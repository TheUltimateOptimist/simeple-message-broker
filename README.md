# Simple Message Broker
## Protokoll
Das zugrundeliegende Transport Protokoll ist UDP.  
Die 4 Typen von Nachrichten, die gesendet werden koennen sind:  
1. Publish  
Aufbau: ptopic;message  
Topic und message muessen mit den jeweiligen Werten ersetzt werden.
2. Subscribe  
Aufbau: stopic  
Topic muss mit dem jeweiligen topic ersetzt werden.  
3. Unsubscribe  
Aufbau: u  
4. von Broker an Subscriber  
Aufbau: topic;message  

Anhand des ersten Buchstaben der Nachricht (p, s oder u) erkennt der Broker,
ob es sich um eine Publish, Subscribe oder Unsubscribe Nachricht handelt.  
Der restliche Teil der Nachricht enthaelt dann eventuell noch Message und Topic, wobei
beide durch Semikolon getrennt werden.

Jede einzelne Nachricht, die im System gesendet wird kann maximal 508 Byte gross sein.  
Laut [dieser Quelle](https://stackoverflow.com/a/35697810) ist das die maximale sichere UDP Payload Groesse.  
Diese Groesse kann theoretisch auch leicht geaendert werden, indem man die Definition von MAX_UDP_PAYLOAD am
Anfang jeder der drei Source Dateien aendert.

## 3 Systemkomponenten
### 1. smbbroker  
Der smbbroker kann ueber die Kommandozeile mit folgenden Optionen ausgefuehrt werden:
|Kurz|Lang|Beschreibung|Default|
|:---|:---|:-----------|:------|
|-?|--help|Diese Hilfeliste anzeigen.|—|
|—|--usage|Kurzinfo zur Benutzung anzeigen.|—|
|-V|--version|Programmversion ausgeben.|—|
|-p|--port|Gibt den Port an, auf dem der Broker gestartet wird.|8080|
|-s|--max-subscribers|Gibt die maximal moegliche Anzahl von parallelen Subscribers an.|100|
|-m|--max-message-length|Gibt die maximale Laenge fuer einzelne Messages an.|500|
|-t|--max-topic-length|Gibt die maximale Laenge fuer einzelne Topics an.|128|

Fuer das Parsen der Kommandozeilenargumente verwendet der Broker [argp (GNU C Library)](https://www.gnu.org/software/libc/manual/html_node/Argp.html).  
Wenn keinerlei Optionen uebergeben wurden, wird der Broker auf Port 8080 mit maximal 100 parallelen
Subscribers und einer maximalen Topic Laenge von 128 und einer maximalen Message Laenge von 500 gestartet.  

Der Broker gibt jede Nachricht, die er erhaelt, zusammen mit der Adresse des Clients auf der Kommandozeile aus.  
Bei Nachrichten, die den maximalen UDP Paylod, die maximale Topic oder Message Laenge ueberschreiten wird zusaetzlich eine Warnung in Gelb auf der Konsole ausgegeben. Diese Nachrichten werden vom Broker nicht weiter verarbeitet.  
Falls eine Subscribe Nachricht empfangen wird, obwohl die Subscriber Liste voll ist, wird ebenfalls eine Warnung auf der Konsole ausgegeben.

Der Broker unterstuetzt hierarchische Topics, mit beliebig vielen Wildcards (\#).  
Wildcards sind allerdings nur fuer Subscribers erlaubt nicht fuer Publishers.

Ganz grob funktioniert der Broker wie folgt:  
Nach dem die Kommandozeilenargumente geparst wurden, legt der Broker eine Subscribers Liste mit der maximal moeglichen Anzahl Subscribers an.  
Anschliessend wird ein Server Socket erstellt, ueber den Nachrichten empfangen werden.  
Im Falle einer Unsubscribe Nachricht, wird die jeweilige Clientadresse in der Subscribers Liste gesucht und der Subscriber wird aus dieser entfernt, indem die gesamte Liste um 1 nach links geshiftet, die Laenge dekrementiert und das Topic des Subscribers freigegeben wird.  
Im Falle einer Subscribe Nachricht wird der Subscriber mit seiner Adresse und dem Topic das er abonniert zur Subscribers Liste hinzugefueht (memcpy). Das topic wird dabei dynamisch auf dem Heap allokiert.  
Im Falle einer Publish Nachricht, wird die publish Funktion mit dem jeweiligen topic und der message aufgerufen. Sie geht dann durch jeden Subscriber in der Subscriber Liste und ueberprueft mit der do_topics_match Funktion, ob der Subscriber die Message erhalten soll und sendet ihm die Message, falls ja.

Der Broker verwendet eine cleanup Funktion, welche die subscribers liste wieder freigibt und den Server Socket schliesst. Diese cleanup Funktion wird mit atexit registriert, sodass sie bei Programmexit aufgerufen wird.  

Der Broker faengt zusaetzlich die folgenden zwei Signale ab:  
1. SIGINT  
In handle_sigint wird globale Variable should_stop auf 1 gesetzt, was dazu fuehrt, dass der Broker die Endlosschleife verlaesst und terminiert.
2. SIGUSR1  
In handle_sigusr1 wird die globale Variable should_clear_subscribers auf 1 gesetzt, was dazu fuehrt, dass der Broker alle vorhandenen Subscriber aus der Subscriber Liste rauswirft, wenn das naechste Mal ein neuer Subscriber subscribed.

Mit Hilfe von setsockopt wird mit der Option SO_RCVTIMEO ein Timeout von 100ms auf das recvFrom vom Server Socket gesetzt. Das fuehrt dazu, dass recvFrom mit errno = EINTR terminiert, wenn ein asynchrones Signal wie SIGINT abgehandelt wird.

### 2. smbpublish
smbpublish kann ueber die Kommandozeile mit folgenden Optionen ausgefuehrt werden:
| Kurz | Lang      | Beschreibung                    | Default   |
| :--- | :-------- | :------------------------------ | :-------- |
| -h   | --host    | IP/HOSTNAME des Brokers.         | 127.0.0.1 |
| -p   | --port    | PORT des Brokers.                | 8080      |
| -?   | --help    | Diese Hilfeliste anzeigen.       | —         |
| —    | --usage   | Kurzinfo zur Benutzung anzeigen. | —         |
| -V   | --version | Programmversion ausgeben.        | —         |

Zusaetzlich muss smbpublish noch mit einem Topic und eventuell einer Message als positionellem Argument aufgerufen werden.  
Wenn keine Message beim Aufruf uebergeben wurde, wird diese von stdin gelesen, falls es an eine Pipe gebunden ist. Ansonsten wird eine leere Message verschickt.   
Dadurch ist es moeglich den Output von anderen Programmen (bspw. ls) an den Aufruf von smbpublish zu pipen.

Beispiele:
|Kommando|Effekt|
|:-------|:-----|
|./smbpublish auto|Leere Message wird unter dem Topic auto ueber den Broker 127.0.0.1:8080 gepublished.|
|./smbpublish auto/rad 4 --host localhost --port 10000|Message 4 wird unter dem Topic auto/rad ueber den Broker 127.0.0.1:10000 gepublished.|
|ls \| smbpublish home/dateien --host 105.3.2.1|Output von ls wird unter dem Topic home/dateien ueber den Broker 105.3.2.1:8080 gepublished.|
|watch "date \| ./smbpublish zeit/jetzt"|Alle 2 Sekunden wird das aktuelle Datum unter dem Topic zeit/jetzt ueber den Broker 127.0.0.1:8080 gepublished.|

Fuer das Parsen der Kommandozeilenargumente verwendet smbpublish [argp (GNU C Library)](https://www.gnu.org/software/libc/manual/html_node/Argp.html).  

smbpublish gibt einen Fehler(in rot) auf der Konsole aus, wenn die Nachricht die maximal zulaessige UDP Payload Groesse von 508 Bytes ueberschreitet.

### 3. smbsubscribe
smbsubscribe kann ueber die Kommandozeile mit den folgenden Optionen ausgefuehrt werden:  
| Kurz | Lang      | Beschreibung                    | Default   |
| :--- | :-------- | :------------------------------ | :-------- |
| -h   | --host    | IP/HOSTNAME des Brokers.         | 127.0.0.1 |
| -p   | --port    | PORT des Brokers.                | 8080      |
| -?   | --help    | Diese Hilfeliste anzeigen.       | —         |
| —    | --usage   | Kurzinfo zur Benutzung anzeigen. | —         |
| -V   | --version | Programmversion ausgeben.        | —         |

Zusaetzlich muss smbsubscribe noch mit einem Topic als positionellem Argument aufgerufen werden.  

Beispiele:
|Kommando|Effekt|
|:-------|:-----|
|./smbsubscribe auto|Alle Nachrichten an das Topic auto werden von dem Broker 127.0.0.1:8080 empfangen/ausgegeben.|
|./smbsubscribe auto/rad --host localhost --port 10000|Alle Nachrichten an das Topic auto/rad werden von dem Broker 127.0.0.1:10000 empfangen/ausgegeben.|
|./smbsubscribe "topic/\#" --host 100.1.1.1|Alle Nachrichten an die Topics topic/\# werden von dem Broker 100.1.1.1:8080 empfangen/ausgegeben.|

Fuer das Parsen der Kommandozeilenargumente verwendet smbsubscribe [argp (GNU C Library)](https://www.gnu.org/software/libc/manual/html_node/Argp.html).

smbsubscribe gibt einen Fehler(in rot) auf der Konsole aus, wenn das uebergebene Topic die maximal zulaessige UDP Payload Groesse von 508 Bytes ueberschreibt

Ganz grob funktioniert smbsubscribe wie folgt:  
Am Anfang wird der uebergebene Host mit gethostbyname resolved und ein Socket fuer die Kommunikation wird angelegt.  
Auf den angelegten Socket wird mit Hilfe von setsockopt und der Option SO_RCVTIMEO ein Timeout von 100ms fuer die recvFrom Aufrufe gelegt.  
Dies dient dazu, dass recvFrom Aufrufe unterbrochen werden, wenn der sigint_handler aufgerufen wurde.  
Ueber den angelgten Socket wird erstmal eine Publish Nachricht an den Broker gesendet.  

Anschliessen geht smbsubscribe in eine while loop, die solange ausgefuehrt wird, bis die globale Variable should_stop auf 1 gesetzt wird. (von sigint_handler)  
Innerhalb der while loop wird staendig ein recvFrom auf dem anfangs angelegten Socket aufgerufen, um Nachrichten vom Broker zu entfangen. Als Adresse wird beim recvFrom Aufruf NULL angegeben.  
Alle empfangenen Nachrichten werden mit \n am Ende auf der Kommandozeile ausgegeben.  
Wenn die while loop irgendwann in Folge von SIGINT verlassen wird, wird noch eine Unsubscribe Nachricht von dem anfangs angelegten Socket an den Broker gesendet. Danach wird der Socket dann geschlossen.








