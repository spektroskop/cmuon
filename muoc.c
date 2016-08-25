#include "muon.h"

int main(int argc, char *argv[]) {
    char cmd[BUFSIZ];
    unsigned n;

    if(argc < 2) d("error: arguments");

    for(unsigned o = 0, c = sizeof(cmd), n = 0; --argc && ++argv && c > 0; o += n, c -= n)
        n = snprintf(cmd + o, c, "%s ", *argv);

    unsigned fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/muon-socket");
    connect(fd, (struct sockaddr *) &addr, sizeof(addr));
    send(fd, cmd, strlen(cmd), 0);

    char res[BUFSIZ];
    if((n = recv(fd, res, sizeof(res), 0)) > 0)
        printf("%s", res);

    close(fd);

    return 0;
}
