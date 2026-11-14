.PHONY: test
test:
	$(CC) -DMEMWATCH -DMW_STDIO test.c memwatch.c mw_recovery.c
