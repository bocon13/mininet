// Example success: sudo ./piper ./test
// Example failure: sudo ./piper -m ./test (mnexec closes the pipe)

#include <unistd.h>
#include <stdio.h>
void main() {
    if (write(4, "test\n", 6) == -1) {
        perror("write failed");
    }
    else printf("write succeeded\n");
}
