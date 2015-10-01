#!/usr/bin/env ruby

# This is library for testing of statsd-aggregator
# Tests are based on simulating behavior of statsd-aggregator and
# analyzing of the output of real implementation (network and stdout)

require 'eventmachine'

# port statsd aggregator listens on
IN_PORT = 9000
# port statsd aggregator writes to
OUT_PORT = 9100
# location of config file for statsd aggregator. This config file is generated for each test run.
CONFIG_FILE = "/tmp/statsd-aggregator.conf"
# location of statsd aggregator executable
EXE_FILE = "../statsd-aggregator"
# how often statsd aggregator flushes data to downstreams
FLUSH_INTERVAL = 2.0

# test exit code in case of success
SUCCESS_EXIT_STATUS = 0
# test exit code in case of failure
FAILURE_EXIT_STATUS = 1
# default test timeout value, can be changed via set_test_timeout() method
DEFAULT_TEST_TIMEOUT = 20

# min and max metrics length (those are processed differently than metrics with garbage content)
MIN_METRICS_LENGTH = 6
MAX_METRICS_LENGTH = 1450

# we are extending String class with numeric? method
# it should return true if string is float number, false otherwise
class String
    def numeric?
        Float(self) != nil rescue false
    end
end

# helper class to handle statsd aggregator output (network or stdout)
class OutputHandler < EventMachine::Connection
    def receive_data(data)
        # test controller is notified with event, which is hash with 2 fields:
        # source - specifies where we got data from
        # data - contains what we got
        @test_controller.notify({source: @source, data: data})
    end

    def initialize(test_controller, source)
        @test_controller = test_controller
        # source should be network or stdout
        @source = source
    end
end

# statsd-aggregatr simulator
# it is using same logic as c version
class StatsdAggregator
    def initialize(statsd_aggregator_test)
        # each slot corresponds to the metric, data with same metric name should go to one and the same slot
        @slots = []
        # how much data we have ready for transfer to the downstream
        @active_buffer_length = 0
        @sat = statsd_aggregator_test
    end

    # simulates flushing data to the downstream
    def flush()
        # let's filter out slots with data
        slots_with_data = @slots.select {|s| ! s[:values].empty? }
        if ! slots_with_data.empty?
            # event, which we 
            @sat.expect({source: "network", data: slots_with_data})
        end
        @slots = []
        @active_buffer_length = 0
    end

    # find slot with given name or create new slot, return index of the slot
    def find_slot(name)
        @slots.each_with_index do |s, i|
            if s[:name] == name
                return i
            end
        end
        if @active_buffer_length + (name.size + 1) > MAX_METRICS_LENGTH
            flush()
        end
        # each slot has following properties:
        # name
        # type (unknown, counter or other)
        # counter - used exclusively for counter aggregation
        # values - list of values for non counter metrics
        @slots << {name: name, type: "unknown", counter: 0.0, values: []}
        @active_buffer_length += (name.size + 1)
        @slots.size - 1
    end

    def insert_values_into_slot(slot_idx, metric)
        slot = @slots[slot_idx]
        # metric is an array [metric_name, metric_value_0, ... metric_value_N]
        # metric_value should be "value|type", or "value|type|@rate"
        name = metric.shift
        metric.each do |m|
            a = m.split("|")
            if a.size == 1
                # metric_value has not enough parts
                @sat.expect({source: "stdout", data: "invalid metric data \"#{m}\""})
                next
            end
            metric_type = "other"
            if a[1] == "c"
                metric_type = "counter"
            end
            if slot[:type] == "unknown"
                # this is newly created slot, setting type
                slot[:type] = metric_type
            else
                # this is existing slot with known type, is new type ok?
                if slot[:type] != metric_type
                    # bad type
                    @sat.expect({source: "stdout", data: "got improper metric type for \"#{name}\""})
                    next
                end
            end
            if @active_buffer_length + (m.size + 1) > MAX_METRICS_LENGTH
                # we have enough data, let's flush
                flush()
                # flush() resets slots, need to create new slot
                @slots << {name: name, type: metric_type, counter: 0.0, values: []}
                @active_buffer_length += (name.size + 1)
                slot = @slots[0]
            end
            if metric_type == "counter"
                # counters are treated differently
                # default rate is 1.0
                rate = 1.0
                if a[2] != nil && a[2][0] == "@" && a[2][1..-1].numeric?
                    # if rate value is valid - use it
                    rate = a[2][1..-1].to_f
                end
                if ! a[0].numeric?
                    # metric value should be numerical, otherwise it's invalid
                    @sat.expect({source: "stdout", data: "invalid value in counter data \"#{m}\""})
                else
                    if slot[:values][0] != nil
                        # if this is not 1st value we need to subtract data length from total length
                        @active_buffer_length -= (sprintf("%.15g|c", slot[:counter]).to_s.size + 1)
                    end
                    # counter value is updated
                    slot[:counter] += (a[0].to_f / rate)
                    # total length is updated
                    @active_buffer_length += (sprintf("%.15g|c", slot[:counter]).to_s.size + 1)
                    # new value is appended to the list
                    slot[:values][0] = sprintf("%.15g|c", slot[:counter]).to_s
                end
            else
                # this is not counter, just append it to the list of values
                slot[:values] << m
                @active_buffer_length += (m.size + 1)
            end
        end
    end

    # processing of single metrics line
    def process_line(s)
        a = s.split(":")
        if a.size == 1
            # no : means no metrics data
            @sat.expect({source: "stdout", data: "invalid metric #{s}"})
        else
            slot_idx = find_slot(a[0])
            insert_values_into_slot(slot_idx, a)
        end
    end

    # simulate network data read
    def read(data)
        data.split("\n").each do |s|
            # metrics lines should fit certain size range
            if s.size.between?(MIN_METRICS_LENGTH, MAX_METRICS_LENGTH)
                process_line(s)
            else
                @sat.expect({source: "stdout", data: "invalid length #{s.size} of metric #{s}"})
            end
        end
    end
end

class StatsdAggregatorTest
    attr_accessor :timeout, :test_sequence

    # this function sends data during test execution
    def send_data_impl(data)
        @sa.read(data)
        @data_socket.send(data, 0, '127.0.0.1', IN_PORT)
    end

    # this function runs actual test
    def run()
        # let's install signal handlers
        Signal.trap("INT")  { EventMachine.stop }
        Signal.trap("TERM") { EventMachine.stop }
        # let's generate config file for statsd aggregator
        File.open(CONFIG_FILE, "w") do |f|
            f.puts("log_level=4")
            f.puts("data_port=#{IN_PORT}")
            f.puts("downstream_flush_interval=#{FLUSH_INTERVAL}")
            f.puts("downstream=127.0.0.1:#{OUT_PORT}")
        end
        # socket for sending data
        @data_socket = UDPSocket.new
        @sa = StatsdAggregator.new(self)
        # here we start event machine
        EventMachine::run do
            # let's start downstream
            EventMachine::open_datagram_socket('0.0.0.0', OUT_PORT, OutputHandler, self, "network")
            # start statsd aggregator
            EventMachine.popen("#{EXE_FILE} #{CONFIG_FILE}", OutputHandler, self, "stdout")
            # and set timer to interrupt test in case of timeout
            EventMachine.add_timer(@timeout) do
                die("Timeout. Stdout: #{@stdout}, expected events: #{@expected_events}")
            end
            sleep 1 # ugly delay to ensure statsd-aggregator started
            @test_sequence.each do |run_data|
                method = run_data[0]
                if method == nil
                    die("No method specified")
                end
                send(method, run_data[1])
            end
            @sa.flush()
            if @expected_events.empty? && @stdout.empty?
                EventMachine.stop()
                exit(SUCCESS_EXIT_STATUS)
            end
            @test_completed = true
        end
    end

    def initialize()
        @test_sequence = []
        @expected_events = []
        @timeout = DEFAULT_TEST_TIMEOUT
        @test_completed = false
        @stdout = ""
        @id = 0
    end

    # called by simulator to add expected events
    def expect(event)
        STDERR.puts("expected: #{event}") if $verbose
        event[:id] = @id
        @id += 1
        @expected_events << event
    end

    # this function is used to notify test of external events
    # if expected event matches actual event it is removed
    def notify(event)
        STDERR.puts "got: #{event}" if $verbose
        events = @expected_events.select {|e| e[:source] == event[:source]}
        case event[:source]
            when "stdout"
                @stdout += event[:data]
                stdout = @stdout.split("\n")
                while true
                    data = stdout.first
                    e = events.first
                    if e && data && data.end_with?(e[:data])
                        @expected_events.delete(e)
                        stdout.shift
                        events.shift
                    else
                        @stdout = stdout.join("\n")
                        break
                    end
                end
            when "network"
                events.each do |e|
                    expected_data = e[:data].map {|m| "#{m[:name]}:#{m[:values].join(":")}"}.sort
                    actual_data = event[:data].split("\n").sort
                    if expected_data == actual_data
                        @expected_events.delete(e)
                    end
                end
            else
                die("Unknown event source: #{event[:source]}")
        end
        # if all expected events matched - test passed ok
        # otherwise it would fail because of timeout
        if @stdout.empty? && @expected_events.empty? && @test_completed
            EventMachine.stop()
            exit(SUCCESS_EXIT_STATUS)
        end
    end

    # this function is used to stop test run in case of error
    def die(reason)
        STDERR.puts "Test failed: #{reason}"
        if EventMachine.reactor_running?
            EventMachine.stop()
        end
        exit(FAILURE_EXIT_STATUS)
    end
end

@sat = StatsdAggregatorTest.new

$verbose = ARGV.delete("-v")

# syntactic sugar start

def set_test_timeout(t)
    @sat.timeout = t
end

def send_data(data)
    @sat.test_sequence << [:send_data_impl, data]
end

# syntactic sugar end

# test configuration is done, now let's run it
at_exit do
    @sat.run()
end
