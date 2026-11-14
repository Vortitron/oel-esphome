# Builds the desktop player: make -C host
render: render.cpp ../components/oel/oel_player.cpp ../components/oel/oel_player.h
	$(CXX) -std=c++17 -O2 -Wall -Wextra -o $@ render.cpp ../components/oel/oel_player.cpp -lz -lm

clean:
	rm -f render

.PHONY: clean
