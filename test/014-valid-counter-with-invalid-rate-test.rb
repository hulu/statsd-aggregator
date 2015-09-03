#!/usr/bin/env ruby

require './statsd-aggregator-test-lib'

send_data("abcdef:1|c|xx\n" * 4)
send_data("abcdef:1|c|@xx\n" * 4)
send_data("abcdef:1|c|@0.1xx\n" * 4)

