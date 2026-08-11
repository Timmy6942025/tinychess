#pragma once
#include <cstdint>
#include <cstdio>
#include <optional>
#include <libchess/Position.h>


class polyglot_book
{
private:
	FILE              *fh  { nullptr };
	size_t             n   { 0       };

	void scan(const libchess::Position & p, const long start_index, const int direction, const long end, std::vector<std::pair<libchess::Move, int> > & moves_out);

public:
	polyglot_book();
	~polyglot_book();

	bool   begin(const std::string & filename);

	size_t size() const;

	std::optional<libchess::Move> query(const libchess::Position & p, const bool verbose);
};
