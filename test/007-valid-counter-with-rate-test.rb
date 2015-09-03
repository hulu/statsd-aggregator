#!/usr/bin/env ruby

require './statsd-aggregator-test-lib'

send_data("abcdef:1|c|@0.1\n" * 4)

