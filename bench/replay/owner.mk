# Use the checkout's configured compiler, optimization and distcc settings.
h_input_owner: bench/replay/input_owner.cc src/gcc_files.cc $(filter-out src/distcc.o,$(distcc_obj))
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I. $(shell pkg-config --cflags fuse3) -std=gnu++17 -o $@ $< $(filter-out src/distcc.o,$(distcc_obj)) $(LDFLAGS) $(LIBS) $(shell pkg-config --libs fuse3) -pthread
