obj-m := cixfan.o

KDIR ?= /lib/modules/$(shell uname -r)/build
IASL ?= iasl

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# acpi/cix_ec_raw.hex is checked in; regenerate it only after editing the ASL.
aml:
	$(IASL) -tc -p acpi/cix_ec_raw acpi/cix-ec-raw.asl

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
