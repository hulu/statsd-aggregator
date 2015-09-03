#!/usr/bin/env ruby

require './statsd-aggregator-test-lib'

send_data("abcdef:1|ms\nabcdefg:1|ms|@0.1\n" * 4)

