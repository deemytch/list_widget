/*
 * Тестовый отправитель: ovpn-send <путь-к-сокету> [строка ...]
 * Отправляет одну корректную передачу и закрывает соединение.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_socket (const char *path){
    struct sockaddr_un address;
    int fd;

    if (strlen (path) >= sizeof address.sun_path){
        fprintf (stderr, "путь к сокету слишком длинный\n");
        exit (EXIT_FAILURE);
    }
    fd = socket (AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0){
        perror ("socket");
        exit (EXIT_FAILURE);
    }
    memset (&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    strcpy (address.sun_path, path);
    if (connect (fd, (struct sockaddr *) &address, sizeof address) < 0){
        perror ("connect");
        exit (EXIT_FAILURE);
    }
    return fd;
}

static void send_all(int fd, const void *data, size_t size){
    const unsigned char *bytes = data;
    size_t done = 0;
    while (done < size){
        ssize_t written = write (fd, bytes + done, size - done);
        if (written <= 0){
            perror ("write");
            exit (EXIT_FAILURE);
        }
        done += (size_t) written;
}}

int main (int argc, char **argv){
    unsigned char header[2] = { 0xFF, 0 };
    unsigned char tail[2] = { 0, 0 };
    int           fd;
    int           index;
    if (argc < 2){
        fprintf (stderr, "использование: %s <путь-к-сокету> [строка ...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    header[1] = (unsigned char) (argc - 2);
    fd = connect_socket(argv[1]);
    send_all(fd, header, sizeof header);

    for (index = 2; index < argc; index++){
        unsigned char length = (unsigned char) strlen (argv[index]);
        send_all(fd, &length, 1);
        send_all(fd, argv[index], length);
    }
    send_all(fd, tail, sizeof tail);
    close (fd);
    return EXIT_SUCCESS;
}
