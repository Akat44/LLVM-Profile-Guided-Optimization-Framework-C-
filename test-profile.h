#pragma once

#include <stdbool.h>
#include <string.h>

bool is_profile_run(int argc, char** argv) {
	for (unsigned i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--profile-data-file=", strlen("--profile-data-file=")) == 0) { return true; }
	}
	return false;
}