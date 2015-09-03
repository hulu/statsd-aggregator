#!/usr/bin/env ruby

require './statsd-aggregator-test-lib'

send_data("abcdef:1|ms\nabcdefg:1|c\n" * 100)
send_data("abcdef:1|ms\nabcdefg:1|c\n" * 100)
send_data("abcdef:1|ms\nabcdefg:1|c\n" * 100)

