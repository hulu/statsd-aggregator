#!/usr/bin/env ruby

# This is library for testing of statsd-aggregator

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

class String
    def numeric?
        Float(self) != nil rescue false
    end
end

# helper class to handle statsd aggregator output
class OutputHandler < EventMachine::Connection
    def receive_data(data)
        @test_controller.notify({source: @source, data: data})
    end

    def initialize(test_controller, source)
        @test_controller = test_controller
        @source = source
    end
end

class StatsdAggregator
    def initialize(statsd_aggregator_test)
        @slots = []
        @active_buffer_length = 0
        @sat = statsd_aggregator_test
    end

    def flush()
        slots_with_data = @slots.select {|s| ! s[:values].empty? }
        if ! slots_with_data.empty?
            @sat.expect({source: "network", data: slots_with_data})
        end
        @slots = []
        @active_buffer_length = 0
    end

    def find_slot(name)
        @slots.each_with_index do |s, i|
            if s[:name] == name
                return i
            end
        end
        if @active_buffer_length + (name.size + 1) > MAX_METRICS_LENGTH
            flush()
        end
        @slots << {name: name, type: "unknown", counter: 0.0, values: []}
        @active_buffer_length += (name.size + 1)
        @slots.size - 1
    end

    def insert_values_into_slot(slot_idx, metric)
        slot = @slots[slot_idx]
        name = metric.shift
        metric.each do |m|
            a = m.split("|")
            if a.size == 1
                @sat.expect({source: "stdout", data: "invalid metric data \"#{m}\""})
                next
            end
            metric_type = "other"
            if a[1] == "c"
                metric_type = "counter"
            end
            if slot[:type] == "unknown"
                slot[:type] = metric_type
            else
                if slot[:type] != metric_type
                    @sat.expect({source: "stdout", data: "got improper metric type for \"#{name}\""})
                    next
                end
            end
            if @active_buffer_length + (m.size + 1) > MAX_METRICS_LENGTH
                flush()
                @slots << {name: name, type: metric_type, counter: 0.0, values: []}
                @active_buffer_length += (name.size + 1)
                slot = @slots[0]
            end
            if metric_type == "counter"
                rate = 1.0
                if a[2] != nil && a[2][0] == "@" && a[2][1..-1].numeric?
                    rate = a[2][1..-1].to_f
                end
                if ! a[0].numeric?
                    @sat.expect({source: "stdout", data: "invalid value in counter data \"#{m}\""})
                else
                    if slot[:values][0] != nil
                        @active_buffer_length -= (sprintf("%.15g|c", slot[:counter]).to_s.size + 1)
                    end
                    slot[:counter] += (a[0].to_f / rate)
                    @active_buffer_length += (sprintf("%.15g|c", slot[:counter]).to_s.size + 1)
                    slot[:values][0] = sprintf("%.15g|c", slot[:counter]).to_s
                end
            else
                slot[:values] << m
                @active_buffer_length += (m.size + 1)
            end
        end
    end

    def process_line(s)
        a = s.split(":")
        if a.size == 1
            @sat.expect({source: "stdout", data: "invalid metric #{s}"})
        else
            slot_idx = find_slot(a[0])
            insert_values_into_slot(slot_idx, a)
        end
    end

    def read(data)
        data.split("\n").each do |s|
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

    def expect(event)
        STDERR.puts("expected: #{event}") if $verbose
        event[:id] = @id
        @id += 1
        @expected_events << event
    end

    # this function is used to notify test of external events
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
