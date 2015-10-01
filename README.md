Statsd-aggregator combines metrics and optimizes network transfers to statsd.

How to compile and install:

$ make install

You also can create deb package (fpm is required):

$ make pkg

Configuration file.

Sample configuration file can be found in /usr/share/statsd-aggregator/statsd-aggregator.conf.sample
Working configuration file location is /etc/statsd-aggregator.conf

data_port - statsd-aggregator would listen on this port (e.g. data_port=8125)
downstream_flush_interval - How often we flush data to the downstream (float value in seconds e.g. downstream_flush_interval=1.0)
downstream - Downstream statsd address:port (e.g. downstream=127.0.0.1:8126)
log_level - How noisy are our logs (4 - error, 3 - warn, 2 - info, 1 - debug, 0 - trace, e.g. log_level=4)

Statsd-aggregator can be controlled via /etc/init.d/statsd-aggregator

How tests work.

To run tests use:

$ make test

Tests are simulating metrics source and check that either correct data is
being send to the statsd downstream or correct message is being logged.
For more details please see test/statsd-aggregator-test-lib.rb
