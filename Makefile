.PHONY: all clean host guest deploy test

CC = gcc
CFLAGS = -Wall -O2 -std=c11
LDFLAGS = -lrt

all: host guest

host: host_writer.c
	$(CC) $(CFLAGS) -o host_writer host_writer.c $(LDFLAGS)

guest: guest_reader.c
	$(CC) $(CFLAGS) -o guest_reader guest_reader.c $(LDFLAGS)

# Deploy guest program to VM
deploy: guest
	@echo "Copying guest_reader to VM..."
	scp -i temp_id_rsa -P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		guest_reader.c debian@localhost:/tmp/
	@echo "Compiling on VM..."
	ssh -i temp_id_rsa -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		debian@localhost 'cd /tmp && gcc -Wall -O2 -std=c11 -o guest_reader guest_reader.c -lrt'
	@echo "Guest program ready at /tmp/guest_reader on VM"

# Run test (starts guest in background via SSH, then runs host)
test: host deploy
	@echo ""
	@echo "Starting guest reader on VM..."
	@ssh -i temp_id_rsa -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		debian@localhost 'sudo /tmp/guest_reader' &
	@echo "Waiting 2 seconds for guest to initialize..."
	@sleep 2
	@echo ""
	@echo "Starting host writer..."
	@./host_writer

clean:
	rm -f host_writer guest_reader
	@ssh -i temp_id_rsa -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		debian@localhost 'rm -f /tmp/guest_reader /tmp/guest_reader.c' 2>/dev/null || true

