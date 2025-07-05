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

// 508 bytes - maximaler, sicherer UDP Payload
// laut dieser Quelle: https://stackoverflow.com/a/35697810
#define MAX_UDP_PAYLOAD 508

// Name der Version (Name der globalen Variable ist von argp vorgeschrieben)
const char *argp_program_version = "smbpublish 1.0.0";

// E-Mail fur Bugs (Name der globalen Variable ist von argp vorgeschrieben)
const char *argp_program_bug_address = "<jonathan.dueck@informatik.hs-fulda.de>";

// Programm Dokumentation
static char doc[] =
    "smbpublish is a small cmd utility for publishing\nmessages under a given topic to a given broker";

// usage beschreibung
static char args_doc[] = "TOPIC MESSAGE\nTOPIC //reads message from stdin";

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
    char *message;
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
        else if (state->arg_num == 1) // zweites positions argument
        {
            arguments->message = arg;
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
    // Programm Argumente Struct mit default werten anlegen
    struct arguments arguments;
    arguments.host = "127.0.0.1";
    arguments.port = 8080;
    arguments.topic = NULL;
    arguments.message = NULL;

    // Parsen der Kommandozeilenargumente mit argp
    // Kann das Programm eventuell bereits terminieren,
    // wenn das Parsen missglueckt
    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    // Resolven des uebegebenen Hosts zu IP Adresse
    struct hostent *hostent = gethostbyname(arguments.host);
    if (hostent == NULL)
    {
        fprintf(stderr, "host resolve failed for: %s\n", arguments.host);
        return 1;
    }

    // +1 wegen \0, dass ich immer am Ende einfuege
    char payload[MAX_UDP_PAYLOAD + 1];

    // sicherstellen, dass topic und message in udp payload passen
    // -2 damit noch platz fuer p fuer publish und ; ist
    char *message = arguments.message != NULL ? arguments.message : "";
    if (strlen(arguments.topic) + strlen(message) > MAX_UDP_PAYLOAD - 2)
    {
        fprintf(stderr, "\033[1;31mError: Topic and message do not fit into the MAX UDP PAYLOAD of 508 bytes.\033[0m\n");
        return 1;
    }
    // Schreiben von topic und message in udp payload
    sprintf(payload, "p%s;%s", arguments.topic, message);

    // Wenn keine message uebergeben wurde, wird sie von stdin gelesen
    // falls stdin von einer pipe kommt, ansonsten bleibt die message leer
    // So koennen andere programme auch eine message ausgeben und sie zu
    // smbpublish pipen
    // bspw. ls | smbpublish dateien/root
    // input wird von stdin in temp gelesen und von dort in payload buffer geschrieben
    if (arguments.message == NULL && !isatty(STDIN_FILENO))
    {
        char temp[1025];
        int message_bytes_read = 0;
        do
        {
            message_bytes_read = read(STDIN_FILENO, temp, sizeof(temp) - 1);
            if (message_bytes_read == -1)
            {
                perror("read");
                return 1;
            }
            temp[message_bytes_read] = '\0';
            if (strlen(payload) + strlen(temp) > MAX_UDP_PAYLOAD)
            {
                fprintf(stderr, "\033[1;31mError: The provided message does not fit into the MAX UDP PAYLOAD of 508 bytes.\033[0m\n");
                return 1;
            }
            strcpy(payload + strlen(payload), temp);
        } while (message_bytes_read > 0);
    }

    // Socket anlegen und oeffnen
    // Familie: Internet, Typ: UDP-Socket
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0)
    {
        perror("socket");
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

    // Nachricht publishen
    int nbytes = sendto(
        sock_fd,
        payload,
        strlen(payload),
        0,
        (struct sockaddr *)&broker_addr,
        sizeof(broker_addr));
    if (nbytes != strlen(payload))
    {
        close(sock_fd);
        perror("sendto");
        return 1;
    }

    close(sock_fd);
    return 0;
}