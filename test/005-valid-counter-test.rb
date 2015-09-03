#!/usr/bin/env ruby

require './statsd-aggregator-test-lib'

send_data("abcdef:1|c\n" * 3)

