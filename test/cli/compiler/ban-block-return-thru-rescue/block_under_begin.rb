# frozen_string_literal: true
# typed: true
# compiled: true

def g
  yield
end

def f
  begin
    g { return 99 }
  ensure
    puts "ensure"
  end
end
