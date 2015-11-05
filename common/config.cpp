#include "config.h"

void exitWithError(const std::string &error) 
{
	std::cout << error;
	std::cin.ignore();
	std::cin.get();

	exit(EXIT_FAILURE);
}



