# Statsd-aggregator

Statsd-aggregator does local aggregation of statsd metrics, lowering the
network traffic and amount of data sent to statsd clusters or instances.

## How it works

Statsd aggregator accepts udp traffic in [statsd](https://github.com/etsy/statsd)
format. Counters are aggregated by summing values, all other metrics are
aggregated by sending all values with same name prefix. Aggregated data is
being fit into packets not exceeding MTU and is flushed to the downstream
according to the flush interval. E.g. if statsd aggregator would get
following data:

```
test.counter:1|c
test.timer:23|ms
test.counter:10|c
test.timer:51|ms
```

it would aggregate it into

```
test.counter:11|c
test.timer:23|ms:51|ms
```

## How to compile and install

Please ensure you have a development version of libev installed

* redhat/centos: `yum install libev-devel`
* debian/ubuntu: `apt-get install libev-dev`

```
$ make install
```

You also can create a deb package (fpm is required):

```
$ make pkg
```

## Configuration file

Sample configuration file can be found in `/usr/share/statsd-aggregator/statsd-aggregator.conf.sample`

Working configuration file location is `/etc/statsd-aggregator.conf`

* data\_port - statsd-aggregator would listen on this port (e.g. data\_port=8125)
* downstream\_flush\_interval - How often we flush data to the downstream (float value in seconds e.g. downstream\_flush\_interval=1.0)
* downstream - Downstream statsd address:port (e.g. downstream=127.0.0.1:8126)
* log\_level - How noisy are our logs (4 - error, 3 - warn, 2 - info, 1 - debug, 0 - trace, e.g. log\_level=4)

Statsd-aggregator can be controlled via `/etc/init.d/statsd-aggregator`

## How tests work

Testing framework is written in ruby and requires evenmachine gem, please
install it using following command:

```
$ sudo gem install eventmachine
```

To run tests use:

```
$ make test
```

Tests are simulating metrics source and check that either correct data is
being send to the statsd downstream or correct message is being logged.
For more details please see `test/statsd-aggregator-test-lib.rb`
