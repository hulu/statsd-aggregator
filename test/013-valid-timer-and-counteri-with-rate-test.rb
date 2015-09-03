#!/usr/bin/env ruby

require './statsd-aggregator-test-lib'

send_data("abcdef:1|ms\nabcdefg:1|c|@0.1\n" * 100)
send_data("abcdef:2|ms\nabcdefg:2|c|@0.2\n" * 100)
send_data("abcdef:3|ms\nabcdefg:3|c|@0.3\n" * 100)

