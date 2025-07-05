#include <stdlib.h>
#include <argp.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/time.h>

// 508 bytes - maximaler, sicherer UDP Payload
// laut dieser Quelle: https://stackoverflow.com/a/35697810
#define MAX_UDP_PAYLOAD 508
// Anzalh Millisekunden nach denen recvFrom abbricht
#define RECVFROM_TIMEOUT_MS 100

// wird in handle_sigint auf 1 gesetzt
// => programm verlaesst while loop
volatile sig_atomic_t should_stop = 0;

void handle_sigint(int sig)
{
    should_stop = 1;
}

// Name der Version (Name der globalen Variable ist von argp vorgeschrieben)
const char *argp_program_version = "smbsubscribe 1.0.0";

// E-Mail fur Bugs (Name der globalen Variable ist von argp vorgeschrieben)
const char *argp_program_bug_address = "<jonathan.dueck@informatik.hs-fulda.de>";

// Programm Dokumentation
static char doc[] =
    "smbpsubscribe is a small cmd utility for subscribing to\nmessages under a given topic from a given broker";

// usage beschreibung
static char args_doc[] = "TOPIC";

// akzeptierte optionen
static struct argp_option options[] = {
    {"host", 'h', "IP/HOSTNAME", 0, "IP/HOSTNAME of Broker (DEFAULT: 127.0.0.1)"},
    {"port", 'p', "PORT", 0, "PORT of Broker (DEFAULT: 8080)"},
    {0} // terminiere liste
};

// Alle Optionen und Positionsargumente
// Werte werden gesetzt wenn argv geparst wird.
struct arguments
{
    char *topic;
    char *host;
    int port;
};

// Parsen der Optionen und Positionsargumente
static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    // Abrufen von arguments struct von argp_state
    struct arguments *arguments = state->input;

    switch (key)
    {
    case 'h':
        arguments->host = arg;
        break;
    case 'p':
        arguments->port = atoi(arg);
        break;
    case ARGP_KEY_ARG:
        // key ARGP_KEY_ARG wird and parse_opt uebergeben wenn parser positions argument
        // erkennt, *arg enthaelt dann das entsprechende argument
        if (state->arg_num == 0) // erstes positions argument
        {
            arguments->topic = arg;
        }
        else // zu viele argumente
        {
            argp_usage(state); // gibt usage information aus und terminiert program
        }
        break;
    case ARGP_KEY_END:
        // key ARGP_KEY_END wird an parse_opt uebergeben, wenn keine argumente mehr uebrig sind
        if (state->arg_num == 0) // ueberpruefe zahl der geparsten positionsargumente
        {
            fprintf(stderr, "Too few arguments: TOPIC is missing.\n");
            argp_usage(state);
        }
        break;

    default: // Zurueckgeben von ARGP_ERR_UNKNOWN terminiert programm und gibt usage infos aus
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

// Erstellen des argp parsers
static struct argp argp = {options, parse_opt, args_doc, doc};

int main(int argc, char **argv)
{
    // Registriere siging handler
    signal(SIGINT, handle_sigint);

    // Programm Argumente Struct mit default werten anlegen
    struct arguments arguments;
    arguments.host = "127.0.0.1";
    arguments.port = 8080;
    arguments.topic = NULL;

    // Parsen der Kommandozeilenargumente mit argp
    // Kann das Programm eventuell bereits terminieren,
    // wenn das Parsen missglueckt
    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    // sicherstellen, dass topic in udp payload passt
    // +1 wegen s fuer subscribe
    if (strlen(arguments.topic) + 1 > MAX_UDP_PAYLOAD)
    {
        fprintf(stderr, "\033[1;31mError: The topic does not fit into the MAX UDP PAYLOAD of 508 bytes.\033[0m\n");
        return 1;
    }

    // Resolven des uebegebenen Hosts zu IP Adresse
    struct hostent *hostent = gethostbyname(arguments.host);
    if (hostent == NULL)
    {
        fprintf(stderr, "host resolve failed for: %s\n", arguments.host);
        return 1;
    }

    // Socket anlegen und oeffnen
    // Familie: Internet, Typ: UDP-Socket
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0)
    {
        perror("socket");
        return 1;
    }

    // recvFrom timeout festlegen
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 1000 * RECVFROM_TIMEOUT_MS;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        close(sock_fd);
        perror("setsockopt");
        return 1;
    }

    // Brokeradresse einrichten
    // Datenstruktur auf 0 setzen
    // Familie: Internetserver
    // Adresse: hostenet
    struct sockaddr_in broker_addr;
    memset((void *)&broker_addr, 0, sizeof(broker_addr));
    broker_addr.sin_family = AF_INET;
    broker_addr.sin_addr = *(struct in_addr *)hostent->h_addr_list[0];
    broker_addr.sin_port = htons(arguments.port);

    // Subscriben
    // + 2 wegen s fuer subscriben und \0 am ende
    char message_send_buffer[strlen(arguments.topic) + 2];
    sprintf(message_send_buffer, "s%s", arguments.topic);
    int bytes_sent = sendto(
        sock_fd,
        message_send_buffer,
        strlen(message_send_buffer),
        0,
        (struct sockaddr *)&broker_addr,
        sizeof(broker_addr));
    if (bytes_sent != strlen(message_send_buffer))
    {
        close(sock_fd);
        perror("sendto");
        return 1;
    }

    // Messages empfangen
    // +2 damit ich am Ende \n und \0 einfuegen kann
    char message_receive_buffer[MAX_UDP_PAYLOAD + 2];
    while (!should_stop) // should_stop wird von handle_sigint auf 1 gesetzt
    {
        int bytes_received = recvfrom(
            sock_fd,
            message_receive_buffer,
            sizeof(message_receive_buffer) - 2,
            0,
            NULL,
            NULL);
        // programm soll beendet werden => while loop wird verlassen
        if (should_stop)
            continue;
        if (bytes_received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // recvFrom timeout abgelaufen => starte neu
            continue;
        }
        else if (bytes_received < 0)
        {
            close(sock_fd);
            perror("recvFrom");
            return 1;
        }
        message_receive_buffer[bytes_received] = '\n';
        message_receive_buffer[bytes_received + 1] = '\0';
        // write message to stdout
        int bytes_written = write(STDOUT_FILENO, message_receive_buffer, strlen(message_receive_buffer));
        if (bytes_written != strlen(message_receive_buffer))
        {
            close(sock_fd);
            perror("write");
            return 1;
        }
    }

    // Unsubscribe
    char unsubscribe_message[] = "u";
    bytes_sent = sendto(
        sock_fd,
        unsubscribe_message,
        strlen(unsubscribe_message),
        0,
        (struct sockaddr *)&broker_addr,
        sizeof(broker_addr));
    if (bytes_sent != strlen(unsubscribe_message))
    {
        close(sock_fd);
        perror("sendto");
        return 1;
    }

    close(sock_fd);
    return 0;
}