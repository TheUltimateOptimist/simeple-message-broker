#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <argp.h>
#include <signal.h>
#include <sys/time.h>

// 508 bytes - maximaler, sicherer UDP Payload
// laut dieser Quelle: https://stackoverflow.com/a/35697810
#define MAX_UDP_PAYLOAD 508
// Anzalh Millisekunden nach denen recvFrom abbricht
#define RECVFROM_TIMEOUT_MS 100

/////////////////////////Hilfsfunktionen///////////////

int sockaddr_in_to_string(struct sockaddr_in *addr, char *buffer, size_t buflen)
{
    char ip[INET_ADDRSTRLEN]; // INET_ADDRSTRLEN = 16 fuer IPv4

    // Konvertiere IP Adresse
    if (inet_ntop(AF_INET, &(addr->sin_addr), ip, sizeof(ip)) == NULL)
    {
        perror("inet_ntop");
        exit(1);
    }

    // Erstelle string mit ip und port
    return snprintf(buffer, buflen, "%s:%d", ip, ntohs(addr->sin_port));
}

// Entscheidet ob subscribed topic sub auf published topic pub passt
// mit ein bisschen pointer arithmetik und strchr/strncmp/strlen
bool do_topics_match(char *pub, char *sub)
{
    char *pub_end = pub + strlen(pub);
    char *sub_end = sub + strlen(sub);
    while (pub < pub_end && sub < sub_end)
    {
        char *pub_forward_slash = strchr(pub, '/');
        char *sub_forward_slash = strchr(sub, '/');
        if (pub_forward_slash == NULL)
        {
            pub_forward_slash = pub + strlen(pub);
        }
        if (sub_forward_slash == NULL)
        {
            sub_forward_slash = sub + strlen(sub);
        }
        if (*sub == '#' && *sub_forward_slash == '\0')
        {
            return true;
        }
        if (*sub == '#' ||
            pub_forward_slash - pub == sub_forward_slash - sub &&
                strncmp(pub, sub, pub_forward_slash - pub) == 0)
        {
            pub = pub_forward_slash + 1;
            sub = sub_forward_slash + 1;
        }
        else
        {
            return false;
        }
    }
    // Falss das subscribte topic nicht ganz verarbeitet wurde
    // passen die topics nicht
    if (sub < sub_end)
    {
        return false;
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////

// Name der Version (Name der globalen Variable ist von argp vorgeschrieben)
const char *argp_program_version = "smbbroker 1.0.0";

// E-Mail fur Bugs (Name der globalen Variable ist von argp vorgeschrieben)
const char *argp_program_bug_address = "<jonathan.dueck@informatik.hs-fulda.de>";

// Programm Dokumentation
static char doc[] =
    "smbbroker is a broker for pub/sub communication between clients.";

// akzeptierte optionen
static struct argp_option options_description[] = {
    {"port", 'p', "PORT", 0, "PORT of Broker (DEFAULT: 8080)."},
    {"max-subscribers", 's', "MAX_SUBSCRIBERS", 0, "Max number of subscribers that can subscribe to broker simultaneously (DEFAULT: 100)."},
    {"max-topic-length", 't', "MAX_TOPIC_LENGTH", 0, "Max length of any topic used by subscribers/publishers (DEFAULT: 128)."},
    {"max-message-length", 'm', "MAX_MESSAGE_LENGTH", 0, "Max length of messages published to the broker (DEFAULT: 500)."},
    {0} // terminiere liste
};

// Alle Optionen von dem Programm
// Werte werden gesetzt wenn argv geparst wird.
struct options
{
    int port;
    int max_subscribers;
    int max_topic_length;
    int max_message_length;
};

// Parsen der Optionen und Positionsargumente
static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    // Abrufen von options struct von argp_state
    struct options *options = state->input;

    switch (key)
    {
    case 'p':
        options->port = atoi(arg);
        break;
    case 's':
        options->max_subscribers = atoi(arg);
        break;
    case 't':
        options->max_topic_length = atoi(arg);
        break;
    case 'm':
        options->max_message_length = atoi(arg);
        break;
    case ARGP_KEY_ARG:
        // key ARGP_KEY_ARG wird and parse_opt uebergeben wenn parser positions argument
        // erkennt, *arg enthaelt dann das entsprechende argument
        // broker akzeptiert keine positions argumente
        argp_usage(state); // gibt usage information aus und terminiert program
        break;
    case ARGP_KEY_END:
        // key ARGP_KEY_END wird an parse_opt uebergeben, wenn keine argumente mehr uebrig sind
        break;
    default: // Zurueckgeben von ARGP_ERR_UNKNOWN terminiert programm und gibt usage infos aus
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

// Erstellen des argp parsers
static struct argp argp = {options_description, parse_opt, NULL, doc};

// Das subscriber struct enthaelt die sockaddr fuer einen
// spezifischen subscriber und einen pointer auf das topic, das er abhoert.
// das topic wird auf dem heap allokiert und muss wieder freigegeben werden
// wenn subscriber unsubscribed
struct subscriber
{
    struct sockaddr_in addr;
    char *topic;
};

int server_fd = -1;

// In main methode wird der subscribers variable eine heap allokierte,
// max subscribers grosse subscribers liste uebergeben
struct subscriber *subscribers = NULL;

int subscribers_length = 0;

// Wrid in handle_sigint auf 1 gesetzt => server schleife wird verlassen => programm terminiert
volatile sig_atomic_t should_stop = 0;

// Wird in handle_sigusr1 auf 1 gesetzt => Das naechste Mal wenn ein neuer Subscriber
// subscribed wird die vorhandene subscriber liste geleert (siehe Funktion clear_subscribers)
volatile sig_atomic_t should_clear_subscribers = 0;

// gibt die subscribers liste frei
void clear_subscribers()
{
    if (subscribers == NULL)
        return;
    for (int i = 0; i < subscribers_length; i++)
    {
        free(subscribers[i].topic);
    }
    free(subscribers);
    subscribers_length = 0;
}

// schliesst socket und gibt dynamisch allokierten Speicher frei
void cleanup()
{
    if (server_fd != -1)
    {
        close(server_fd);
        server_fd = -1;
    }
    clear_subscribers();
}

void handle_sigint(int sig)
{
    // fuehrt dazu das server endlosschleife verlaesst
    should_stop = 1;
}

void handle_sigusr1(int sig)
{
    // fuehrt dazu, dass die subscriber liste komplett
    // erneuert wird, wenn der naechste subscriber subscribed
    should_clear_subscribers = 1;
}

// publish durchlaeft die komplette subscriber liste und ueberprueft welche subscriber
// das uebergebene topic subscriben und leitet ihnen das topic und die message weiter
// die message ist in der uebergebenen topic variable enthalten, wenn man das erste \0 entfernt : )
void publish(char *topic)
{
    for (int i = 0; i < subscribers_length; i++)
    {
        if (do_topics_match(topic, subscribers[i].topic))
        {
            // topic und message werden mit ; getrennt an broker gesendet, welcher
            // dieses dann durch \0 ersetzt
            // Bevor ich aber an den subscriber weiterleite, ersetze ich das \0 wieder durch
            // ein semikolon um topic und message zu kriegen
            // und topic und message zu subscriber zu senden
            size_t topic_length = strlen(topic);
            topic[topic_length] = ';'; // topic enthaelt ab jetzt auch message
            int bytes_sent = sendto(
                server_fd, topic,
                strlen(topic),
                0,
                (struct sockaddr *)&(subscribers[i].addr),
                sizeof(struct sockaddr_in));
            if (bytes_sent != strlen(topic))
            {
                perror("sendto");
                exit(1);
            }
            topic[topic_length] = '\0'; // topic enthaelt nicht mehr message
        }
    }
}

int main(int argc, char **argv)
{
    // Registrieren von singal handlers
    signal(SIGUSR1, handle_sigusr1); //=> clear subscribers
    signal(SIGINT, handle_sigint);   //=> beende programm

    atexit(cleanup); // register cleanup function

    // Programm Options Struct mit default werten anlegen
    struct options options;
    options.port = 8080;
    options.max_subscribers = 100;
    options.max_topic_length = 128;
    options.max_message_length = 500;

    // Parsen der Kommandozeilenargumente mit argp
    // Kann das Programm eventuell bereits terminieren,
    // wenn das Parsen missglueckt
    argp_parse(&argp, argc, argv, 0, 0, &options);

    // Allokieren von subscribers liste mit max-subscribers
    subscribers = (struct subscriber *)malloc(options.max_subscribers * sizeof(struct subscriber));
    memset((void *)subscribers, 0, options.max_subscribers * sizeof(struct subscriber));

    // Anlegen des Buffers fuer den UDP Server
    char buffer[MAX_UDP_PAYLOAD + 1]; // +1 fuer \0

    // udp socket erstellen
    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        exit(1);
    }

    // recvFrom timeout festlegen
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 1000 * RECVFROM_TIMEOUT_MS;
    if (setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        perror("setsockopt");
        exit(1);
    }

    // Serverstruktur einrichten und an Port binden
    // Datenstruktur auf 0 setzen
    // Familie: Internetserver
    // Adresse: beliebige Clientadressen zulassen
    // Port: wie in options.port festgelegt
    struct sockaddr_in server_addr;
    memset((void *)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(options.port);
    bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    while (!should_stop)
    {
        // Erhalte Client Message
        struct sockaddr_in client_addr;
        socklen_t client_addr_size = sizeof(client_addr);
        int bytes_received = recvfrom(
            server_fd,
            buffer,
            sizeof(buffer) - 1, // -1 damit \0 nachher nicht ausserhalb des buffers eingefuegt wird
            MSG_TRUNC,          // ueberpruefen ob datagramm MAX_UDP_PAYLOAD ueberschreitet
            (struct sockaddr *)&client_addr,
            &client_addr_size);

        if (bytes_received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            // recvFrom timeout abgelaufen (EAGAIN || EWOULDBLOCK)
            // recvFrom durch asynchrones Signal unterbrochen (EINTR)
            continue;
        }
        else if (bytes_received < 0)
        {
            perror("recvFrom");
            exit(1);
        }

        if (bytes_received > MAX_UDP_PAYLOAD)
        {
            fprintf(
                stderr,
                "\033[1m\033[33mWarning: Message exceeds MAX UDP PAYLOAD of %d bytes.\033[0m\n",
                MAX_UDP_PAYLOAD);
            buffer[MAX_UDP_PAYLOAD] = '\0';
        }
        else
            buffer[bytes_received] = '\0'; // null terminate the input

        // Ausgeben von Client Adresse und Nachricht in STDOUT
        char client[128];
        sockaddr_in_to_string(&client_addr, client, 128);
        fprintf(stderr, "%s:%s\n", client, buffer);

        // Protokoll abarbeiten
        // Subscribe: message = s<topic>
        // Unsubscribe: message = u
        // Publish: message = p<topic>;<content>
        if (buffer[0] == 's')
        {                                 // subscribe
            if (should_clear_subscribers) // should_clear_subscribers kann in handle_sigusr1 gesetzt werden_
            {
                clear_subscribers();
                // Allokieren von subscribers liste mit max-subscribers
                subscribers = (struct subscriber *)malloc(options.max_subscribers * sizeof(struct subscriber));
                memset((void *)subscribers, 0, options.max_subscribers * sizeof(struct subscriber));
            }
            if (subscribers_length >= options.max_subscribers)
            { // sicherstellen, dass das max subscribers limit eingehalten wird
                fprintf(
                    stderr,
                    "\033[1m\033[33mWarning: Subscribe failed because MAX_SUBSCRIBERs LIMIT of %d was exceeded.\033[0m\n",
                    options.max_subscribers);
            }
            else if (strlen(buffer) < 2)
            {
                fprintf(stderr, "\033[1m\033[33mWarning: Subscribe failed because no topic was provided.\033[0m\n");
            }
            else if (strlen(buffer) > options.max_topic_length)
            { // sicherstellen, dass max topic length eingehalten wird
                fprintf(
                    stderr,
                    "\033[1m\033[33mWarning: Subscribe failed because MAX TOPIC LENGTH of %d was exceeded.\033[0m\n",
                    options.max_topic_length);
            }
            else
            {
                // Add new subscriber to subscribers list
                char *topic = malloc(strlen(buffer + 1) + 1);
                memcpy((void *)&(subscribers[subscribers_length].addr), (void *)&client_addr, client_addr_size);
                memcpy((void *)topic, buffer + 1, strlen(buffer + 1) + 1);
                subscribers[subscribers_length].topic = topic;
                subscribers_length++;
                fprintf(stderr, "\033[36mCurrent Subscribers: %d/%d\033[0m\n", subscribers_length, options.max_subscribers);
            }
        }
        else if (buffer[0] == 'u') // unsubscribe
        {
            // finde subscriber and entferne ihn von subscribers liste
            for (int i = 0; i < subscribers_length + 1; i++)
            {
                if (i == subscribers_length)
                {
                    fprintf(stderr, "\033[1m\033[33mWarning: Unsubscribe failed because the client wasn't found in subscribers list.\033[0m\n");
                }
                else if (
                    memcmp(
                        (void *)&(subscribers[i].addr),
                        (void *)&client_addr,
                        client_addr_size) == 0)
                {
                    char *subscriber_topic = subscribers[i].topic;
                    // shifte ganze subscribers liste 1 nach links um
                    // subscriber zu entfernen
                    for (int l = i + 1; l < subscribers_length; l++)
                    {
                        memcpy(
                            (void *)&(subscribers[l - 1]),
                            (void *)&(subscribers[l]),
                            sizeof(struct subscriber));
                    }
                    subscribers_length--;
                    free(subscriber_topic);
                    fprintf(stderr, "\033[36mCurrent Subscribers: %d/%d\033[0m\n", subscribers_length, options.max_subscribers);
                    break;
                }
            }
        }
        else if (buffer[0] == 'p') // publish
        {
            char *semicolon = strchr(buffer + 1, ';');
            if (semicolon == NULL)
            {
                fprintf(
                    stderr,
                    "\033[1m\033[33mWarning: Publish failed because the message is malformed, message: %s.\033[0m\n",
                    buffer);
            }
            *semicolon = '\0';
            char *topic = buffer + 1;
            char *message = semicolon + 1;
            if (strlen(topic) > options.max_topic_length)
            { // sicherstellen, dass max topic length eingehalten wird
                fprintf(
                    stderr,
                    "\033[1m\033[33mWarning: Publish failed because MAX TOPIC LENGTH of %d was exceeded.\033[0m\n",
                    options.max_topic_length);
            }
            else if (strlen(message) > options.max_message_length)
            { // sicherstellen das max message length eingehalten wird
                fprintf(
                    stderr,
                    "\033[1m\033[33mWarning: Publish failed because MAX MESSAGE LENGTH of %d was exceeded.\033[0m\n",
                    options.max_message_length);
            }
            else
            {
                publish(buffer + 1); // +1 um buchstaben der publish anzeigt loszuwerden
            }
        }
    }
    exit(0);
}