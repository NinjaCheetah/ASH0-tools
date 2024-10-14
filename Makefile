# Subdirectories to build
SUBDIRS := Compressor Decompressor

.PHONY: all clean

all:
	for dir in $(SUBDIRS); do \
	    $(MAKE) -C $$dir; \
	done

debug:
	for dir in $(SUBDIRS); do \
	    $(MAKE) -C $$dir debug; \
	done

clean:
	for dir in $(SUBDIRS); do \
	    $(MAKE) -C $$dir clean; \
	done
