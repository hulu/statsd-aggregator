PKG_NAME=statsd-aggregator
PKG_VERSION=0.0.1
PKG_DESCRIPTION="Local aggregator for statsd metrics"

.PHONY: all test clean

all: bin
bin:
	gcc -Wall -O2 -I/usr/include/libev -o statsd-aggregator statsd-aggregator.c -lev
clean:
	rm -rf statsd-aggregator build
pkg: bin
	mkdir build
	cp -r etc build/
	cp -r usr build/
	mkdir build/usr/bin/
	cp statsd-aggregator build/usr/bin/
	cd build && \
	fpm --deb-user root --deb-group root -d libev-dev --description $(PKG_DESCRIPTION) -s dir -t deb -v $(PKG_VERSION) -n $(PKG_NAME) `find . -type f` && \
	rm -rf `ls|grep -v deb$$`
test: bin
	cd test && ./run-all-tests.sh
install: bin
	cp statsd-aggregator /usr/bin
	mkdir -p /usr/share/statsd-aggregator && cp usr/share/statsd-aggregator/statsd-aggregator.conf.sample /usr/share/statsd-aggregator
	cp etc/init.d/statsd-aggregator /etc/init.d/
