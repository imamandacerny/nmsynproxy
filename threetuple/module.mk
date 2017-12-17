THREETUPLE_SRC_LIB := threetuple.c
THREETUPLE_SRC := $(THREETUPLE_SRC_LIB) threetupletest.c

THREETUPLE_SRC_LIB := $(patsubst %,$(DIRTHREETUPLE)/%,$(THREETUPLE_SRC_LIB))
THREETUPLE_SRC := $(patsubst %,$(DIRTHREETUPLE)/%,$(THREETUPLE_SRC))

THREETUPLE_OBJ_LIB := $(patsubst %.c,%.o,$(THREETUPLE_SRC_LIB))
THREETUPLE_OBJ := $(patsubst %.c,%.o,$(THREETUPLE_SRC))

THREETUPLE_DEP_LIB := $(patsubst %.c,%.d,$(THREETUPLE_SRC_LIB))
THREETUPLE_DEP := $(patsubst %.c,%.d,$(THREETUPLE_SRC))

CFLAGS_THREETUPLE := -I$(DIRHASHLIST) -I$(DIRMISC) -I$(DIRHASHTABLE)
LIBS_THREETUPLE := $(DIRMISC)/libmisc.a $(DIRHASHTABLE)/libhashtable.a

MAKEFILES_THREETUPLE := $(DIRTHREETUPLE)/module.mk

.PHONY: THREETUPLE clean_THREETUPLE distclean_THREETUPLE unit_THREETUPLE $(LCTHREETUPLE) clean_$(LCTHREETUPLE) distclean_$(LCTHREETUPLE) unit_$(LCTHREETUPLE)

$(LCTHREETUPLE): THREETUPLE
clean_$(LCTHREETUPLE): clean_THREETUPLE
distclean_$(LCTHREETUPLE): distclean_THREETUPLE
unit_$(LCTHREETUPLE): unit_THREETUPLE

THREETUPLE: $(DIRTHREETUPLE)/libthreetuple.a $(DIRTHREETUPLE)/threetupletest

unit_THREETUPLE: $(DIRTHREETUPLE)/hashtest
	$(DIRTHREETUPLE)/hashtest

$(DIRTHREETUPLE)/libthreetuple.a: $(THREETUPLE_OBJ_LIB) $(MAKEFILES_COMMON) $(MAKEFILES_THREETUPLE)
	rm -f $@
	ar rvs $@ $(filter %.o,$^)

$(DIRTHREETUPLE)/threetupletest: $(DIRTHREETUPLE)/threetupletest.o $(DIRTHREETUPLE)/libthreetuple.a $(LIBS_THREETUPLE) $(MAKEFILES_COMMON) $(MAKEFILES_THREETUPLE)
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^) $(filter %.a,$^) $(CFLAGS_THREETUPLE)

$(THREETUPLE_OBJ): %.o: %.c %.d $(MAKEFILES_COMMON) $(MAKEFILES_THREETUPLE)
	$(CC) $(CFLAGS) -c -o $*.o $*.c $(CFLAGS_THREETUPLE)

$(THREETUPLE_DEP): %.d: %.c $(MAKEFILES_COMMON) $(MAKEFILES_THREETUPLE)
	$(CC) $(CFLAGS) -MM -MP -MT "$*.d $*.o" -o $*.d $*.c $(CFLAGS_THREETUPLE)

clean_THREETUPLE:
	rm -f $(THREETUPLE_OBJ) $(THREETUPLE_DEP)

distclean_THREETUPLE: clean_THREETUPLE
	rm -f $(DIRTHREETUPLE)/libthreetuple.a $(DIRTHREETUPLE)/threetuple

-include $(DIRTHREETUPLE)/*.d
