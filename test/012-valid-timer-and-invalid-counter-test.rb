#!/usr/bin/env ruby

require './statsd-aggregator-test-lib'

send_data("abcdef:1|ms\nabcdefg:qq|c\n" * 100)
send_data("abcdef:1|ms\nabcdefg:qq|c\n" * 100)
send_data("abcdef:1|ms\nabcdefg:qq|c\n" * 100)

