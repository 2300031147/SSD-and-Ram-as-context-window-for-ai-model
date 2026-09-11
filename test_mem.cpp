#include <unistd.h>
#include <iostream>
int main() {
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    std::cout << "Avail: " << (uint64_t)pages * page_size << std::endl;
    return 0;
}
