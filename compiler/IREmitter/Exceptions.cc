#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"

#include "cfg/CFG.h"
#include "compiler/Core/CompilerState.h"
#include "compiler/IREmitter/IREmitterContext.h"
#include "compiler/IREmitter/IREmitterHelpers.h"
#include "compiler/IREmitter/Payload.h"

using namespace std;

namespace sorbet::compiler {

namespace {

class ExceptionState {
    CompilerState &cs;
    llvm::IRBuilder<> &builder;
    const IREmitterContext &irctx;
    const int rubyBlockId;
    const int bodyRubyBlockId;
    const int handlersRubyBlockId;
    const int ensureRubyBlockId;
    const int elseRubyBlockId;
    cfg::LocalRef exceptionValue;
    llvm::Function *currentFunc;

    llvm::Value *previousException = nullptr;
    llvm::BasicBlock *exceptionEntry = nullptr;
    llvm::Value *exceptionResultPtr = nullptr;

    ExceptionState(CompilerState &cs, llvm::IRBuilderBase &builder, const IREmitterContext &irctx, int rubyBlockId,
                   int bodyRubyBlockId, cfg::LocalRef exceptionValue)
        : cs(cs), builder(static_cast<llvm::IRBuilder<> &>(builder)), irctx(irctx), rubyBlockId(rubyBlockId),
          bodyRubyBlockId(bodyRubyBlockId), handlersRubyBlockId(bodyRubyBlockId + cfg::CFG::HANDLERS_BLOCK_OFFSET),
          ensureRubyBlockId(bodyRubyBlockId + cfg::CFG::ENSURE_BLOCK_OFFSET),
          elseRubyBlockId(bodyRubyBlockId + cfg::CFG::ELSE_BLOCK_OFFSET), exceptionValue(exceptionValue),
          currentFunc(irctx.rubyBlocks2Functions[rubyBlockId]) {}

public:
    // Setup the context for compiling exception-handling code, and bring some needed constants into scope.
    static ExceptionState setup(CompilerState &cs, llvm::IRBuilderBase &builder, const IREmitterContext &irctx,
                                int rubyBlockId, int bodyRubyBlockId, cfg::LocalRef exceptionValue) {
        ExceptionState state(cs, builder, irctx, rubyBlockId, bodyRubyBlockId, exceptionValue);

        // Allocate a place to store the exception result from sorbet_try. This must go in the function init block,
        // otherwise the allocation might happen inside of a loop, causing a stack overflow.
        auto ip = state.builder.saveIP();
        state.builder.SetInsertPoint(irctx.functionInitializersByFunction[rubyBlockId]);
        state.exceptionResultPtr = state.builder.CreateAlloca(llvm::Type::getInt64Ty(cs), nullptr, "exceptionValue");
        state.builder.restoreIP(ip);

        // Store the last exception state
        state.previousException = state.builder.CreateCall(cs.getFunction("rb_errinfo"), {}, "previousException");

        // Setup the exception handling entry block
        state.exceptionEntry = llvm::BasicBlock::Create(cs, "exception-entry", state.currentFunc);
        state.builder.CreateBr(state.exceptionEntry);
        state.builder.SetInsertPoint(state.exceptionEntry);

        // Clear out the variable that we store the current exception in
        Payload::varSet(cs, exceptionValue, Payload::rubyNil(state.cs, state.builder), builder, irctx, rubyBlockId);

        return state;
    }

    llvm::Function *getDoNothing() const {
        return cs.getFunction("sorbet_blockReturnUndef");
    }

    llvm::Function *getEnsure() const {
        auto ensurePresent = irctx.rubyBlockType[ensureRubyBlockId] != FunctionType::Unused;
        return ensurePresent ? irctx.rubyBlocks2Functions[ensureRubyBlockId] : getDoNothing();
    }

    llvm::Function *getElse() const {
        auto elsePresent = irctx.rubyBlockType[elseRubyBlockId] != FunctionType::Unused;
        return elsePresent ? irctx.rubyBlocks2Functions[elseRubyBlockId] : getDoNothing();
    }

    llvm::Function *getHandlers() const {
        return irctx.rubyBlocks2Functions[handlersRubyBlockId];
    }

    // Fetch the pc, and closure values that are used when calling an exception function.
    tuple<llvm::Value *, llvm::Value *, llvm::Value *> getExceptionArgs() {
        auto *pc = builder.CreateLoad(irctx.lineNumberPtrsByFunction[rubyBlockId]);
        auto *closure = Payload::buildLocalsOffset(cs);
        auto *cfp = Payload::getCFPForBlock(cs, builder, irctx, rubyBlockId);
        return {pc, closure, cfp};
    }

    // Run a function that may raiase exceptions.
    tuple<llvm::Value *, llvm::Value *> sorbetTry(llvm::Function *fun, llvm::Value *exceptionContext) {
        auto [pc, closure, cfp] = getExceptionArgs();
        auto *result = builder.CreateCall(cs.getFunction("sorbet_try"),
                                          {fun, pc, closure, cfp, exceptionContext, exceptionResultPtr}, "result");

        return {result, exceptionResultPtr};
    }

    // Run the ensure clause, overwriting the previous return value that was passed in if it's present.
    llvm::Value *sorbetEnsure(llvm::Value *previousReturnValue) {
        auto [pc, closure, cfp] = getExceptionArgs();
        auto *res = builder.CreateCall(getEnsure(), {pc, closure, cfp}, "ensureResult");
        auto *notUndef = builder.CreateICmpNE(res, Payload::rubyUndef(cs, builder), "ensureReturnValue");
        return builder.CreateSelect(notUndef, res, previousReturnValue);
    }

    void raiseIfNotNil(llvm::Value *exceptionValue) {
        builder.CreateCall(cs.getFunction("sorbet_raiseIfNotNil"), {exceptionValue});
    }

    // Returns the exception value that was raised.
    llvm::Value *runBody() {
        // Call the body, using sorbet_try to catch any exceptions raised and communicate them out via
        // exceptionResultPtr.
        auto *bodyFunction = irctx.rubyBlocks2Functions[bodyRubyBlockId];
        auto [bodyResult, exceptionResultPtr] = sorbetTry(bodyFunction, previousException);

        // Check for an early return from the body.
        auto *earlyReturnBlock = llvm::BasicBlock::Create(cs, "exception-body-return", currentFunc);
        auto *continueBlock = llvm::BasicBlock::Create(cs, "exception-body-continue", currentFunc);
        auto *isReturnValue = builder.CreateICmpNE(bodyResult, Payload::rubyUndef(cs, builder), "isReturnValue");
        builder.CreateCondBr(isReturnValue, earlyReturnBlock, continueBlock);

        // If a non-undef value was returned from the body, there's no possibility that an exception was raised. Run
        // ensure and return the body result.
        builder.SetInsertPoint(earlyReturnBlock);
        builder.CreateCall(cs.getFunction("rb_set_errinfo"), {previousException});
        auto ensureResult = sorbetEnsure(bodyResult);
        IREmitterHelpers::emitReturn(cs, builder, irctx, rubyBlockId, ensureResult);

        // Update the exceptionValue closure variable to hold the exception raised.
        builder.SetInsertPoint(continueBlock);
        auto *exceptionResult = builder.CreateLoad(exceptionResultPtr);
        Payload::varSet(cs, exceptionValue, exceptionResult, builder, irctx, rubyBlockId);

        return exceptionResult;
    }

    llvm::Value *determinePostRescueExceptionContext(llvm::Value *handlerException) {
        auto *handlerRaised = builder.CreateICmpNE(handlerException, Payload::rubyNil(cs, builder));

        auto *exn = Payload::varGet(cs, exceptionValue, builder, irctx, rubyBlockId);
        auto *exceptionNotHandled = builder.CreateICmpNE(exn, Payload::rubyNil(cs, builder));

        // There are three exceptions that need to be considered when determining the state after a rescue handler has
        // been applied. They're listed in order of preference when being restored for the ensure handler:
        //
        // 1. An exception raised by the rescue block
        // 2. The original exception raised by the body, that wasn't handled by the rescue block
        // 3. The ambient previous exception that was present before exception handling began
        auto *unhandledOrPrevious = builder.CreateSelect(exceptionNotHandled, exn, previousException);
        return builder.CreateSelect(handlerRaised, handlerException, unhandledOrPrevious);
    }

    void runRescueElseEnsure(llvm::Value *exceptionResult) {
        auto *handlersFunction = getHandlers();
        auto *elseFunction = getElse();

        // Run the handler with sorbet_handle, so that we cleanup the vm exception state when it exits successfully.
        auto *exceptionRaised = builder.CreateICmpNE(exceptionResult, Payload::rubyNil(cs, builder));
        auto *handler = builder.CreateSelect(exceptionRaised, handlersFunction, elseFunction, "handler");
        auto [handlerResult, handlerExceptionPtr] = sorbetTry(static_cast<llvm::Function *>(handler), exceptionResult);

        // Setup the exception state for the ensure handler
        auto *handlerException = builder.CreateLoad(handlerExceptionPtr);
        auto *exceptionContext = determinePostRescueExceptionContext(handlerException);
        builder.CreateCall(cs.getFunction("rb_set_errinfo"), {exceptionContext});

        // Check the handlerResult for a retry, and branch back to the beginning of exception handling if it's present
        // and we ran the rescue block.
        // NOTE: if retry was returned from the handler, we know that no new exceptions were raised (exceptionContext
        // will be nil), and that the previous exception will have been restored.
        auto *ensureBlock = llvm::BasicBlock::Create(cs, "exception-ensure", currentFunc);
        auto *shouldRetry = builder.CreateAnd(
            exceptionRaised,
            builder.CreateICmpEQ(handlerResult, Payload::retrySingleton(cs, builder, irctx), "shouldRetry"));
        builder.CreateCondBr(shouldRetry, exceptionEntry, ensureBlock);

        // the handler didn't retry, run ensure and return its value.
        builder.SetInsertPoint(ensureBlock);
        auto *continueBlock = llvm::BasicBlock::Create(cs, "exception-continue", currentFunc);
        auto *returnBlock = llvm::BasicBlock::Create(cs, "exception-return", currentFunc);
        auto *ensureResult = sorbetEnsure(handlerResult);
        auto *isReturnValue = builder.CreateICmpNE(ensureResult, Payload::rubyUndef(cs, builder), "isReturnValue");
        builder.CreateCondBr(isReturnValue, returnBlock, continueBlock);

        // return the result of the ensure
        builder.SetInsertPoint(returnBlock);
        IREmitterHelpers::emitReturn(cs, builder, irctx, rubyBlockId, ensureResult);

        // Re-raise if an exception was raised by the handler
        builder.SetInsertPoint(continueBlock);
        raiseIfNotNil(exceptionContext);
    }

    // If no rescue clause handled the exception, the exceptionValue will contain a non-nil value. Test for that at
    // the end of exception handling, conditionally re-raising it to the outer context.
    void raiseUnhandledException() {
        auto *exn = Payload::varGet(cs, exceptionValue, builder, irctx, rubyBlockId);
        raiseIfNotNil(exn);
    }
};

} // namespace

void IREmitterHelpers::emitExceptionHandlers(CompilerState &cs, llvm::IRBuilderBase &build,
                                             const IREmitterContext &irctx, int rubyBlockId, int bodyRubyBlockId,
                                             cfg::LocalRef exceptionValue) {
    auto state = ExceptionState::setup(cs, build, irctx, rubyBlockId, bodyRubyBlockId, exceptionValue);

    auto exceptionResult = state.runBody();
    state.runRescueElseEnsure(exceptionResult);
    state.raiseUnhandledException();

    return;
}

} // namespace sorbet::compiler
