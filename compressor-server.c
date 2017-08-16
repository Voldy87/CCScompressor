/*Author: Andrea Orlandi
 * name: compressor-server.c
 * date: 28/02/2017
 * description: lato server per il progetto di programmazione distribuita del corso universitario di 
 * 				Organizzazione dei Sistemi Operativi e Reti (OSOR), CdL Triennale in Ingegneria 
 * 				Informatica dell'Università di Pisa, tenuto dal prof. Anastasi.
 * 				Le caratteristiche principali dell'applicazione "remote-compressor" sono:
 * 					- paradigma client-server
 * 					- server concorrente multi-threaded (thread POSIX): POOL_DIMENSION thread Server ed 1 thread Listener
 * 					- comunicazione tramite Berkeley socket TCP ("stream")
 * 				   - compressione tramite il comando "tar"
 * 					- utilizzo dei segnali (ISO C library signals)
 * 					- utilizzo delle espressioni regolari (POSIX ERE)
 * language: Italian (program, comments), English (code)
 * notes: 1) programma scritto per l'esecuzione sotto ambienti UNIX e *nix
 *        2) compilare con l'opzione "-pthread"
 *        3) avviare il server [eventualmente in background] ( "compressor-server <porta> [&] ")
 * 	  4) per terminare il server inviargli SIGINT una volta che tutti i client si sono disconnessi
 *	  5) il programma crea nella directory corrente una cartella contenente tante subdirectory quanti sono i thread del pool [vedi macro "POOL_.."]   
*/

/*  STRUTTURA DEL DOCUMENTO: 
		- librerie (base, segnali, socket, pthreads, regex, directory)
		- macro (pool, archivi, listen, regex, messaggi, versione, colori)
		- typedef (archiviazione, lista di nomi)
		- variabili globali (sincronizzazione, compressione)
		- funzioni (stringhe, socket, sync, regex, tar, funzioni del server, comandi del client e loro parametri)
		- gestori segnali (SIGINT)
		- codice thread (poolserver, listenerserver)
		- codice processo (compressorserver)
*/


/*      LIBRERIE    */
#include <stdio.h>      // librerie base
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>     // per i segnali 
#include <sys/types.h>  // per i socket 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>   // per i POSIX pthreads (man pthreads)
#include <regex.h>     // per le espressioni regolari 
#include <dirent.h>    // per le cartelle 



/*  MACRO  */
#define POOL_DIMENSION 4              // dimensione del pool di Thread Servers (n° massimo di client serviti contemporanamente)
#define POOL_ROOT_DIR "PoolFolders"      // cartella locale del compressore lato server
#define POOL_FOLDER_PREFIX "T"	         // prefisso al numero d'ordine delle cartelle per i file temporanei dei thread del pool (ognuno serve un client)

#define BACKLOG 100                     // per la coda della listen [si veda https://www.freebsd.org/cgi/man.cgi?query=listen&sektion=2]

#define DEFAULT_ARCHIVE_NAME "archivio" // nome di default dell'archivio che creo con la "compress"
#define DEFAULT_COMPRESSOR_INDEX 0      // gnuzip (0 è l'indice di riga, nella matrice dei compressori, relativo a tale algoritmo)
#define NUM_COMPRESSORS 4               // n° algoritmi di compressione supportati dal programma (devono comunque essere installati nel SO)
#define MAX_COMPR_NAME_LENGTH 10        // lunghezza dell'archivio con il nome più lungo, arrotondata per eccesso al multiplo di 10 più vicino


#define FILEPATH_REGEX "([^ \n]+)|([^ \"\n]*(\"[^\"]*\"|[^ \"\n]))+" // espressione regolare path dei file della send [POSIX  Extended Regular Syntax (ERE)]

#define MAX_MSG_LEN 200  // dimensione massima dei messaggi che può inviare il client

#define VERSION "6.3" // versione del programma

#define CYAf  "\x1B[36m"    /* colori */
#define GREf  "\x1B[32m"         // testo
#define REDf  "\x1B[31m" 
#define WHIf  "\x1B[37m"
#define YELf  "\x1B[33m"
#define MAGb  "\x1B[45m"         // sfondo
#define REDb  "\x1B[41m" 
#define RST   "\033[0m"	    	   // reset	


/*     NUOVI TIPI       */

typedef struct  compr_parameters {  /* contiene il nome dell'archivio e il codice del compressore utilizzato */
		int compressor_index;   // 0-gnuzip, 1-bzip2, 2-xz, 3-compress (vedi compressors_matrix)...[gnuzip/0 default]
		char* archive_name;     // punterà alla stringa con il nome da dare all'archivio ["archivio" default] 
	} comp_param;
	
	
typedef struct element { /* elemento base della lista dei nomi dei file da inviare (comando SEND [file]): è una lista dinamica di stringhe. */
		char* str;
		struct element* next;
	} elem;

typedef elem* list;	 /* per semplificare la scrittura delle funzioni che operano sulla suddetta lista */



/*   VARIABILI GLOBALI     */	
	pthread_mutex_t mutex;	     // per mutua esclusione su condizione
	pthread_cond_t PoolSleep;    // attesa risveglio di un ServerThread quando il Listener Thread vuol fornirgli un client (uso broadcast->no sem)
	pthread_cond_t PoolBusy;     // attesa risveglio del ListenerThread quando tutti i ServerThread sono occupati con i clienti
	pthread_cond_t PoolReady;      // in fase di inizializzazione del pool indica l'attesa in operazioni preliminari di tutti i ServerThread
	pthread_cond_t ClientAssigned; // attesa che il singolo ServerThread del pool riceva dal Listener i dati del client
   int in_service;          /* quanti thread del pool stanno servendo un client (da 0 a POOL_DIMENSION)*/
   int c_sock_array;        /* socket descriptor del connected socket da assegnare ad un ServerThread (passaggio del sd tra Listener e Server)*/
   struct sockaddr_in c_sockaddr_array; // indirizzo del client da comunicare ad un ServerThread (passaggio tra Listener e Server)	
	int closing;      // 1 = è stata ordinata la chiusura (ordinata) del server; 0 = tutto procede normalmente
	int ss;           // ci copio il socket_descriptor del listen_sock, così il sighandler può chiuderlo, sbloccando così il ListenerThread sull'accept
	int ReadyThreads; // quanti pool thread hanno completato le operazioni di inizializzazione (al termine delle quali il ListenerThread si sveglia)
	int assignedFlag; // 0= ListenerThread non ha completato l'assegnazione di un client a un PoolThread; 1= tutti i client hanno un PoolThread
	char compressors_matrix[NUM_COMPRESSORS][3][MAX_COMPR_NAME_LENGTH]= { //  3 colonne e tante righe quanti sono i compressori supportati (via tar)
		{"gnuzip", "gz", "z"}, 
		{"bzip2", "bz2", "j"},  
		{"xz", "xz", "J"},
		{"compress", "Z", "Z"}
	};  // nome compressore ,  estensione(senza ".") , opzione per il comando tar 
 
 

/*     FUNZIONI (con eventuale indicazione di come interpretare il ritorno: di solito      */

// funzioni (2) sulle stringhe [uguali per client e server]

char *trim_side_spaces ( char *s )  /* toglie gli spazi a sx e dx della stringa [s] e la restituisce */	
{ 
  int i=0, k=0, j=(strlen(s)-1);
  while ( isspace(s[i]) && s[i]!='\0' )
    i++;
  while ( isspace(s[j]) && j>= 0 )
    j--;
  while ( i<=j )
    s[k++] = s[i++];
  s[k]='\0';
  return s;
}

char *del_chars ( char *r, char x )  /* toglie tutte le occorrenze del char [x] nella stringa [r]  e restituisce [r] modificata */	
{
  int i=0, k=0;
  char s[strlen(r)+1];
  strcpy(s,r);
  for (i=0; s[i]!='\0'; i++) 
		if (s[i]!=x)
			r[k++]=s[i];
  r[k]='\0';
  return r;
}


// funzioni (2) sui socket: 1-ok, 0-errore [uguali per client e server]
   /* quando c'è una dall'altra parte della connessione c'è l'altra: esse fanno tx dimensione dati-> rx dimensione dati -> tx dati -> rx dati */
int SendData ( int sock, const void *data, size_t dim )  /* invio la quantita' [dim] di dati puntati da [data] a [sock] */
{ 
    int total = 0, rc, n, bytesleft = dim;
    rc = send( sock, &dim, sizeof(int), 0 ); // invio al ricevente la dimensione totale dai dati da inviare
    if ( (rc==-1) || (rc<sizeof(int)) )
		return 0;
    while( total < dim ) { 				  // invio finche' non ho spedito tutti i dati
        n = send( sock, data+total, bytesleft, 0 );
        if (n == -1) 
	       	break;    
        total += n;     				 // aggiornamento sui dati inviati e rimanenti
        bytesleft -= n; 
    }				
    return (n==-1)?0:1;  				 // errore su una send: esco 
} 

int ReceiveData ( int sock, void *data, int *len ) /* ricevo da [sock] mettendo dove punta [data]; ne scrivo la quantita' dove punta [len], se non è NULL */
{												
    int total = 0, rc, n, dim, bytesleft; 
    rc = recv( sock, &dim, sizeof(int), MSG_WAITALL ); // ricevo la dimensione dei dati che saranno spediti
    if ( (rc==-1) || (rc<sizeof(int)) )     
	 	return 0;
    bytesleft = dim;
    while( total < dim ) {  			      	// ricevo finche' non ho avuto tutti i dati
        n = recv( sock, data+total, bytesleft, 0 );
        if (n == -1) 
		 		break;
        total += n;      			         // aggiornamento sui dati ricevuti e ancora da ricevere
        bytesleft -= n; 
    }
    if (len!=NULL)       			         // in molti casi il ricevente sa di certo quanti dati arrivano e quindi mette NULL a [3°arg]
		*len=dim;
    return (n==-1)?0:1; 				        // esito della funzione (1->ok, 0->no)
} 


// funzioni (4) su semafori, thread e variabili globali (condivise)

void create_pool ( int *taskids[], pthread_t *threads, void *(*thread_code)(void *) )  /* il ListenerThread crea un POOL_DIMENSION ServerThreads */
{                                                      /* I Poolthread sono puntati da [threads], identificati da [taskids) e  eseguono [thread_code] */
	int t;	      // conterra' i progressivi [0,1..] PoolID dei vari ServerThread del Pool
	ReadyThreads=0; // ancora non ci sono Threads del pool, dunque 0 sono pronti (è incrementato nelle azioni iniziali dei thread appena creati)        
	pthread_cond_init(&PoolSleep, NULL);   // inizializzazione dei semafori
	pthread_cond_init(&PoolBusy, NULL);
	pthread_cond_init(&PoolReady, NULL);
	pthread_cond_init(&ClientAssigned, NULL);       
	in_service=0;                         // inizialmente non ci sono ServerThread del pool che sono pronti a servire un client 
	for (t=0; t<POOL_DIMENSION;t++){   // ATTENZIONE: stiamo assegnando a ciascuno thread un PoolID [da 0 a (POOL_DIMENSION-1)]    
	    taskids[t] = (int *) malloc(sizeof(int)); // Intero assegnato a ogni ServerThread (lo decide ListenerThread creandolo) ma NON è il suo TID
	    *taskids[t] = t;
	    if (pthread_create(&threads[t], NULL, thread_code, (void *) taskids[t])<0) { //creazione singolo thread del pool
		     char *sret = malloc(30);
		     fprintf (stderr, REDf"Errore di creazione del thread %d."RST"\n", t);
		     strcpy(sret,"Pool thread creation error");		
		     pthread_exit((void*)sret);     	//se fallisce la creazione d'un solo thread ServerThread termina e riporta l'errore (join del main)
	    } 	
	}
	pthread_mutex_lock(&mutex);
	while (ReadyThreads<POOL_DIMENSION)             // attendo che tutti i ServerThread si siano bloccati in attesa di un cliente >
		pthread_cond_wait(&PoolReady, &mutex);  // > (mi sveglia l'ultimo che diventa pronto con la Signal nel suo codice)
	pthread_mutex_unlock(&mutex);
}

void assign_client ( struct sockaddr_in c_addr, int c_sock )  /* con essa ListenerThread sveglia 1 ServerThread del pool (in attesa su PoolBusy)  >   */
{	                                                      /* > e gli passa IP:PORT del client [c_addr] e socket connesso ad esso[c_sock])         */
	pthread_mutex_lock(&mutex);            // il client che il Listener (chiamante) sta servendo è in attesa del suo ServerThread (da prendere dal pool)
	while (in_service==POOL_DIMENSION)       // controllo il numero di thread occupati e se lo sono tutti mi blocco in attesa che uno si liberi 
		pthread_cond_wait(&PoolBusy, &mutex); // attende bloccato che si liberi un PoolThread (lo Segnala la ciclica wait&start)       
	assignedFlag=0; 
	c_sockaddr_array=c_addr; // permetto al thread del pool, che sveglierò dopo, di vedere IP#port del client che servirà [info senza utilità pratica] 
	c_sock_array=c_sock;   	 // permetto al thread del pool, che sveglierò dopo, di vedere il conn. socket locale per comunicare col client che servirà 
	pthread_cond_signal(&PoolSleep); // sveglio un singolo PoolThread (random), cui verrà assegnato il client; era in attesa sulla wait&start   
	while (assignedFlag==0)  // Listener attende che il ServerPoolThread svegliato abbia preso in consegna il client (lo segnale settando assignedFlag)
		pthread_cond_wait(&ClientAssigned, &mutex); 
	pthread_mutex_unlock(&mutex); 
}
 
void wait_and_start( int* sock, struct sockaddr_in *addr, int id ) /* il ServerThread [id]-esimo del pool attende la sveglia e poi memorizza > */
{                                                               /* >  i parametri del client (IPa+PORTn[addr] e sd per parlarci[sock])      */
	pthread_mutex_lock(&mutex);
	if ( in_service==(POOL_DIMENSION-1) ) {    // ramo eseguito solo se dopo aver finito di servire un client il thread e' l'unico libero
		pthread_cond_signal(&PoolBusy);        // se  ListenerThread attendeva che un ServerThread si liberasse per assegnargli un client lo sveglio
	}
	while ( (assignedFlag==1) && (closing==0) ) // l'attesa finisce quando mi vogliono assegnare un client o quando SIGINT attiva la chiusura del server
		pthread_cond_wait(&PoolSleep, &mutex); // aspetto che ListenerThread mi svegli (mi assegna una richiesta) - la wait libera da se' il mutex
	if (closing==0) {         //ASSEGNAMENTO DEL CLIENT AL THREAD          
		*sock = c_sock_array;        		   // memorizzo localmente al thread il connected socket da usare e i dati sul client da servire
		*addr= c_sockaddr_array; 
		in_service++;
		printf(REDf"CLIENT "RST"%s"REDf" connesso.", inet_ntoa(addr->sin_addr));
		if (in_service==POOL_DIMENSION)		
			printf(" [servito dal thread "RST"%d"REDf": "YELf"tutti i %d thread sono occupati"REDf"]"RST"\n", id, POOL_DIMENSION);
		else
			printf(" [servito dal thread "RST"%d"REDf": "YELf"%d"REDf"/%d liberi]"RST"\n", id, (POOL_DIMENSION-in_service), POOL_DIMENSION);
		assignedFlag=1;			         // settando questo flag segnalo al Listener (che dopo sveglio) d'aver preso in consegna il client
		pthread_cond_signal(&ClientAssigned);    //sveglio il ListenerThread in attesa che questo thread abbia preso in consegna il client
	}       //se il risveglio  è quello collettivo dovuto alla chiusura totale (via SIGINT) non faccio nulla (non ci sono client da servire)
	pthread_mutex_unlock(&mutex);
}

void ListenerSock_and_Sem_Destroy( int list_sock )  /* il ListenerThread elimina tutti i semafori e chiude il socket di ascolto [list_sock] */
{
	pthread_mutex_destroy(&mutex);        // distruzione dei semafori 
	pthread_cond_destroy(&PoolSleep);
	pthread_cond_destroy(&PoolBusy);
	pthread_cond_destroy(&PoolReady);
	pthread_cond_destroy(&ClientAssigned); 
	if (close(list_sock)<0)    	     // chiusura del listening socket
		perror("close");
}


// funzioni (1) per la compressione

char* tar_cmd (comp_param p, int PoolID) /* crea il comando di compressione con parametri [p] dei files nella cartella locale del Thread [PoolID]-esimo */
{
	int i = p.compressor_index;              // indice dell'algoritmo da usare
	char* s = malloc( (MAX_MSG_LEN+50)*sizeof(char) );
	sprintf(s, "cd %s/%s%d && tar -c", POOL_ROOT_DIR, POOL_FOLDER_PREFIX, PoolID); // entro nella cartella personale e inizio il comando  tar
	strcat(s,compressors_matrix[i][2]);  // opzione compressore da usare (e.g. "z" se uso bzip2)
	strcat(s,"f \"");                    // opzione file
	strcat(s,p.archive_name);            // nome archivio (tutto tra virgolette)
	strcat(s,".tar.");             	      // prima parte estensione archivio (uguale per tutti)
	strcat(s, compressors_matrix[i][1]); // estensione relativa al compressore (e.g. "xz" se uso xz")
	strcat(s, "\" * && cd .. && cd .."); // comprimo tutti i file (nella cartella locale); infine torno alla directory dove gira il processo
	return s;     //comando completo pronto per la system()
}


// funzioni (2) per le espressioni regolari [da http://www.lemoda.net/c/unix-regex/] 

int compile_regex ( regex_t *r, const char *regex_text ) /* prepara l'espressione regolare e dà  0 se tutto ok, 1 altrimenti */ 
{
    int status = regcomp (r, regex_text, REG_EXTENDED|REG_NEWLINE);
    if (status != 0) {
		char error_message[MAX_MSG_LEN];
		regerror (status, r, error_message, MAX_MSG_LEN);
      printf (REDf"Errore nella compilazione dell\'espressione regolare '%s': %s"RST"\n",regex_text, error_message);
      return 1;
    }
    return 0;
}

int match_regex (regex_t *r,  char *to_match, list *li) /* trova i path con regex [r] nella stringa [to_match], li mette in lista [li] e ne ritorna il n°*/
{
    char *p =  to_match; 	          // puntatore alla fine dell'ultimo match
    const int n_matches = MAX_MSG_LEN/2;  // numero massimo di corrispondenze consentite (essendo i path separati da spazi)
    regmatch_t m[n_matches];   		  // array delle corrispondenze trovate 
    int count=0;		         // inizialmente non ho trovato corrispondenze
    char buf[MAX_MSG_LEN];
    while (1) {                           // ciclo finchè non esaurisco le corrispondenze 
        list z;
        int start, finish;
        int nomatch = regexec (r, p, n_matches, m, 0); 
        if (nomatch) 
		 break;
        if (m[0].rm_so != -1) {
            start = m[0].rm_so + (p - to_match);
            finish = m[0].rm_eo + (p - to_match);
            sprintf (buf, "%.*s", (finish - start), to_match + start);// %[flags][width][.precision][length]specifier
	    strcpy(buf,trim_side_spaces(buf));    
	    if (strchr ( buf, '\"' )!=NULL)         
	        strcpy ( buf,  del_chars(buf,'\"') );
	    strcpy(to_match,trim_side_spaces(to_match));
	    z = malloc(sizeof(elem));     
	    z->str = malloc((strlen(buf)+1)*sizeof(char));   // inserimento in testa del primo percorso trovato
	    strcpy(z->str, buf);
	    z->next = (*li);
	    (*li) = z;    
	    count++;
        }
        p += m[0].rm_eo;
    }	
    return count;
}


// funzioni (5) sulle stringhe che contengono i comandi e i loro parametri

void getpar ( char *cmdString, int Len ) /* estrapola il parametro da "<comando> [parametri]" [cmdString]; [Len] è la lunghezza del comando più 1, e > */
{        /* > sostituisce la suddetta stringa con quella formata dai/l parametri/o (sovrascrivendo [cmdString])                     */
	int l = strlen(cmdString);		   
	char temp[l-Len+1]; 
	strncpy( temp, cmdString+Len, l-Len+1);   // copio solo la parte (iniziale) di stringa che contiene la keyword del comando (e.g. "send","compress")
	strcpy( cmdString, temp );
	strcpy( cmdString, trim_side_spaces(cmdString) ); // levo eventuali spazi bianchi ai lati
}

list create_path_list ( char pathString[], int *pathNumber ) /* il parametro della send [elenco] è una stringa con 1+ path di file da inviare  separati > */
{  /* > da  " ": li isolo con le regex, creo una lista e ne ritorno il pointer e la lunghezza [pathNumber], grazie al quale so  quante send fare*/
    list p = NULL;	   
    regex_t r;
    const char* regex_text = FILEPATH_REGEX;   	 	 // espressione regolare per un generico percorso file
    strcpy( pathString, trim_side_spaces(pathString)); 
    compile_regex(&r, regex_text);             		 // compilo l'espressione regolare  
    (*pathNumber) = match_regex(&r, pathString, &p); // conterrà la lunghezza della lista  generata dalla funzione
    regfree (&r);
    return p;  			         // restituisce la testa della lista
}

char *extract_path ( list *pointer )  /* estrae dalla testa [pointer] della lista il path (stringa) di un file da inviare; non ho bisogno > */
{				      /* > del controllo sulla lista vuota perchè in tal caso tale funzione non viene chiamata              */
	list a = *pointer;
	char *z;           
	z = malloc ( (strlen(a->str)+1)*sizeof(char) ); // creo la stringa che conterrà il percorso del file
	strcpy(z, a->str);  			        // copio il path nella stringa che la funzione ritornerà
	(*pointer) = (*pointer)->next;  	        // scorro la lista di una posizione (estrazione in testa)
	free(a);           			        // elimino l'elemento
	return z;				        // ritorno la stringa di testa al chiamante
}

char *getfilename ( char *path )  /* dato in ingresso il [path] (/../../xxx) di un file da inviare ne restituisce il nome ("nome[.estensione]") */
{								        
	int i, pos=(-1);
	char *fn;
	for ( i=0; path[i]!='\0'; i++ ) {   // scorro  tutta la stringa-percorso
		if (path[i]=='/')	         // se trovo uno slash("/") ne memorizzo la posizione
			pos=i;
	}
	if (pos==-1) {	       	// se il percorso non conteneva "/" allora era locale e dato solo dal nome del file, che restituisco	
		fn = malloc ( (strlen(path)+1)*sizeof(char) );
		strcpy(fn, path);
	}
	else {		       // altrimenti restituisco soltanto la parte dopo l'ultimo "/", cioè il nome del file
		fn = malloc ( (strlen(path)+1-pos)*sizeof(char) );
		strncpy( fn , path+pos+1 , strlen(path)+1-pos );
	}
	return fn;    // per finire ritorno la stringa che contiene il nome del file da inviare
}										 

int identify_command ( char *word, char *parameter ) /* data la [word] digitata ritorna l'indice assegnato al comando e eventuali parametri [parameter] */
{ /* Gli indici sono Help:1, Config-compr[]:2, Config-name[]:3, Show-config:4, Send[]:5, Compr[]:6, Show-list:7, Empty-list:8, Quit:9; O ALTRIMENTI  */   
	int l, i; 
	word = trim_side_spaces(word);      // levo gli spazi inutili
	l = strlen(word);
	for ( i=0; (word[i]!=' ') && (word[i]!='\0'); i++ ) // i comandi non sono case-sensitive (lo restano nomi di file, path e metodi di compressione!)
		word[i] = tolower(word[i]);
	if (l<4) 	         // "quit" e "help" sono i comandi più corti disponibili
		return 0;	
	if (l==4){ 
		if (strcmp(word, "help")==0) 
			return 1;
		if (strcmp(word, "quit")==0) 
			return 9;
		return 0;
	} 
	if (strncmp(word, "send ",5)==0) {
		strcpy(parameter,word);
		getpar(parameter, 5);   // tale funzione sostituirà la stringa con comando e parametro con il solo parametro
		return 5; 
	}
	if (strncmp(word, "compress ",9)==0) {
		strcpy(parameter,word);
		getpar(parameter, 9);
		return 6;
	}
	if (strncmp(word, "show-list",9)==0) 
		return 7;
	if (strncmp(word, "empty-list",10)==0) 
		return 8;
	if (strncmp(word, "configure-name ",15)==0){ 
		strcpy(parameter,word);
		getpar(parameter, 15);
		return 3;
	}
	if (l==18){
		if (strncmp(word, "show-configuration",18)==0) 
			return 4;
		return 0;
	}
	if (strncmp(word, "configure-compressor ",21)==0){ 
			strcpy(parameter,word);
			getpar(parameter, 21);
			return 2;
	}
	return 0;        // se arrivo qui significa che non è stato digitato un comando errato o inesistente
}


// funzioni (9) invocate dai ServerThread ("sXXX") in risposta alle richieste del client (il 1° argomento è sempre il suo socket [client_socket]); >
// > tutte ritornano: 0[tutto ok]  -1[il client non risponde]    1[il parametro del comando è errato o altri errori]                             

int sINVALIDCOMMAND ( int client_socket )   /* corrispettivo sul client: cCMDS0_478 [0 è il n° associato ad un comando non esistente] */
{
	char info[MAX_MSG_LEN+1];				         // conterrà la stringa da inviare al client per la stampa video
	strcpy(info, REDf" - Comando inesistente o mancante di parametro.\n"RST); // messaggio relativo al prompt di comando errato o inesistente
	return ( SendData(client_socket, &info, strlen(info)) -1 );      // lunghezza stringa da stampare e Send(non invio il NUL, risparmio 1B)
}

int sHELP ( int client_socket)   /* Corrispettivo sul client: cCMDS0_478{1-help} */
{
	char info[MAX_MSG_LEN*2];
	sprintf(info, GREf" - I comandi supportati da remote-compressor sono i seguenti:\n"
							"%4c-> configure-compressor [compressor]\n"
							"%4c-> configure-name [name]\n"
							"%4c-> show-configuration\n"
							"%4c-> send [local-file]\n"
							"%4c-> compress [path]\n"
							"%4c-> show-list\n"
							"%4c-> empty-list\n"
							"%4c-> quit"RST
							"\n",' ',' ',' ',' ',' ',' ',' ',' '); // "%4c" inserisce 4 volte il char specificato (lo spazio)
	return ( SendData(client_socket, &info, strlen(info)) -1 );         // 1) invio del messaggio (non inviando il NUL risparmio 1B) 
}

int sCONFIGURECOMPRESSOR ( int client_socket, char compr[], comp_param *p )  /* Corrispettivo sul client: cCMDS0_478{2: configure-compressor}. */
{ /* [compr]: nome compr. scelto (può non essere disponibile); [p] punta una struct con l'indice del compr. da usare e il nome archivio in uso   */
	int i;                                                
	char info [110 + (NUM_COMPRESSORS*MAX_COMPR_NAME_LENGTH)]; // al massimo dovrà contenere il messaggio che elenca tutti i compressori disponibili
	for (i=0; i<NUM_COMPRESSORS; i++) { 				       // guardo se il compressore scritto dal client è tra quelli disponibili
		if ( strcmp(compr, compressors_matrix[i][0])==0) { 	   // il compressore specificato è tra quelli usabili 
			p->compressor_index=i;                             // imposto il nuovo compressore di default
			sprintf( info, CYAf" - Compressore configurato correttamente a "GREf"%s"CYAf"."RST"\n", compr );
			break; 		      	   // trovato il compressore è inutile continuare (e l'intero "i" finisce con un valore minore di 4!)
		}
	}
	if (i==NUM_COMPRESSORS) {  	             // vero solo se il compressore specificato è inesistente (i.e. il for prima non ha fatto break)
		char temp [ 10 + MAX_COMPR_NAME_LENGTH ];          // contiene una riga dell'elenco di tutti compressori, che voglio far vedere al client
		strcpy(info,REDf" - Errore sul nome del compressore scelto; i compressori disponibili sono:"RST"\n"REDf); //creazione messaggio "errore"
		for (i=0; i<NUM_COMPRESSORS; i++) {     // elenco compressori disponibili
			sprintf (temp, "   * %s\n", compressors_matrix[i][0]); 
			strcat( info, temp );		         // aggiungo una riga all'elenco
		}
	}									
	if ( ! SendData(client_socket, info, strlen(info)) )       // 1) invio ( byte de)l messaggio, NUL finale escluso, con gestione errore
		return (-1);
	if (i==NUM_COMPRESSORS) 
		return 1;    	         // il compressore specificato non è tra quelli disponibili (il messaggio inviato al client ne contiene l'elenco)
	else
		return 0;   	          // il compressore è stato impostato correttamente (il messaggio inviato al client conferma l'esito positivo)
}

int sCONFIGURENAME ( int client_socket, char* chosenName, comp_param *p ) /* Corrispettivo sul client: cCMDS0_478{3: configure-name}. */   
{   /*  [chosenName] punta al nome  da dare al tar, scelto dal client; [p] punta alla struct contenente quello vecchio, da sostituire con [chosenName]  */
	char info[MAX_MSG_LEN];			   
	int i=0, s=0; 
	while(chosenName[i]!='\0') {    	// gestione errore su nome "vuoto"
		if (chosenName[i]==' ')
			s++;
		i++;
	}
	if (strlen(chosenName)==s) {	        // gestione errore sul nome (tutti spazi bianchi)
		sprintf(info, REDf" - Non è possibile indicare un file con un nome di soli spazi o vuoto"RST"\n");
		SendData(client_socket, &info, strlen(info)); 						// 1) invio messaggio con gestione errore
		return 1;
	}
	else {		       	// tutto ok: il nome va bene
		free(p->archive_name);		        	// cancello l'elemento puntato
		p->archive_name = malloc( (strlen(chosenName)+1)*(sizeof(char)) );	// ne creo uno di dimensioni acconce al inserito dal client
		strcpy( p->archive_name, chosenName );  // scrivo il nuovo nome di default che la compress deve usare per creare il tar coi files della send
		sprintf(info,CYAf" - Nome configurato correttamente a "GREf"%s"CYAf"."RST"\n", chosenName);
		return ( SendData(client_socket, &info, strlen(info)) -1 ); 		// 1) invio messaggio con gestione errore
	}
}

int sSHOWCONFIGURATION ( int client_socket, comp_param *p )  /* Corrispettivo sul client: cCMDS0_478{4: show-configuration}.  */	
{ /* [p] punta la struct coi valori previsti per il compressore (individuato tramite indice della compressor_matrix) e il nome da dare al tar      */
	char info[MAX_MSG_LEN+1];  // conterrà il messaggio stampare a video sul client (inviato via socket); suppone che MAX_MSG_LEN> MAX_COMPR_NAME_LENGTH
	strcpy(info,CYAf"  Nome: "GREf);  	  // creazione messaggio da spedire al client con i parametri impostati attualmente
	strcat(info, p->archive_name);	        	// nome archivio
	strcat(info, CYAf"\n  Compressore: "GREf);      // prosecuzione messaggio
	strcat(info, compressors_matrix[p->compressor_index][0]);   	// algoritmo di compressione
	strcat(info, "\n"RST);											// infine a capo
	return ( SendData(client_socket, &info, strlen(info)) -1 ); // 1) invio messaggio sui parametri in uso per la compressione; gestione errore inclusa
}

int sSEND ( int client_socket, char parameter[], int PoolID, int* counter ) /* Corrispettivo client: cSEND. [parameter] è  il path del  file da inviare */
{ /* [PoolID] identifica il thread che esegue sSend (chiamante); il puntatore a [counter] (n° di file inviati finora dal thread [PoolID]-esimo)> */
	FILE *fp;       	/* > Se l'invio si conclude con successo in [parameter] il chiamante troverà il nome del file inviato */
	char info[MAX_MSG_LEN+1], temp[MAX_MSG_LEN/4];
	char *dyn, *filename, *filepath;                                               /*RICEZIONE FILE INVIATO DAL CLIENT E SUA MEMORIZZAZIONE*/
	unsigned int size; 			   	  // usando un intero C senza segno per la dimensione del file questo potrà essere al massimo 4GiB
	int risp, l;  
	if ( !SendData(client_socket, parameter, strlen(parameter)) ) // 1) invio al client path del file da inviare [".../../../nome[.estensione]"] 
		return -1;                                              
	if ( ! ReceiveData(client_socket, &risp, NULL) )              // 2) il client mi comunica se il file verrà inviato (1) o meno (0) 
		return -1;
	if (risp==0)   	          // se il client non è in grado di accedere al file (non esiste a quel path, oppure non è un file) esco
		return 1;		
	filename = getfilename(parameter); // prelevo dal path il nome del file ("nome[.estensione]"); getfilename mi dà il pointer a una stringa dinamica
	l = strlen(filename);		
	filepath = malloc( (l+50)*(sizeof(char)) );     	  // creazione percorso del file inviato (salvato nella cartella locale del thread) 
	sprintf(filepath, "./%s/%s%d/%s", POOL_ROOT_DIR, POOL_FOLDER_PREFIX, PoolID, filename);
	risp = access(filepath, F_OK);  	    // se il file (nella cartella locale del poool thread) c'è gia access=0, se non c'è access=-1          
	if ( !SendData(client_socket, &risp, sizeof(int)) )    // 3) comunico al client se possiamo procedere (-1) oppure se il file è già stato inviato (0)
		return -1;	
	if (risp==0)         									  // se il file è già stato inviato la funzione termina
		return (1);                                  
	if ( ! ReceiveData(client_socket, &size, NULL) ) 			  // 4) ricezione dimensione file
		return -1;
	if (size!=0){	                                  //se il file è vuoto è tutto più semplice (conta solo il suo nome, che ho già)
	    if ( ! ReceiveData(client_socket, &risp, NULL) )          	  // 5[opz]) il client è in grado di aprire il file?
		    return -1;
	    if (risp==0)					        // il client non riesce ad aprire il file locale che mi vuoel spedire  esco
		    return 1;				
		dyn = malloc(size);			        	  // vi metto momentaneamente il contenuto del file che mi manderà il client
		if ( ! ReceiveData(client_socket, dyn, NULL) )  	    // 6[opzionale se file nn vuoto]) ricezione contenuto del file (va in dyn)	
			return -1;
	}
	fp = fopen(filepath, "wb");      // apertura del file (se ne crea uno nuovo) in cui sarà copiato il contenuto del file "inviato" (sta in dyn)
	if(fp==NULL) { 			        	  // gestione errore di apertura del file (copia locale del file inviatomi dal client)
		fprintf (stderr, REDf"Impossibile creare il file."RST"\n"); 
		strcpy(info,YELf"CLIENT: il server non e' stato in grado di creare il file; invio fallito."RST"\n");                                        
		return SendData(client_socket, &info, strlen(info))-1;	  // 7e) informo il client sulla mancata creazione del file
	}
	if (size!=0)
		fwrite(dyn, 1, size, fp);    // trasferimento (scrittura) del contenuto (del file) inviato dal client nel file, aperto, creato lato server
	fflush(fp);
	fclose(fp);				      	  // chiudo il file: ora il thread server ha nella sua cartella locale il file inviato dal client
	if (size!=0)
		free(dyn);	        // se l'avevo creato cancello il buffer dove avevo appoggiato i dati ricevuti (il contenuto del file inviato)	  
	(*counter)++;             									  // tutto ok: posso incrementare il contatore
	if ((*counter)==1) 
		strcpy(temp,CYAf"("GREf"1"CYAf" file inviato).\n"RST);
	else                                // comunico al client che l'invio è andato a buon fine e quanti file ha inviato in totale 
		sprintf(temp,"("GREf"%d"CYAf" file inviati).\n"RST, (*counter)); 
	strcpy(info,"- File "GREf);
	strcat(info, filename);
	strcat(info,CYAf" inviato con successo "RST);
	strcat(info, temp); 										  // creazione del messaggio da mandare al client
	if ( !SendData(client_socket, &info, strlen(info)) ) 	  // 7) informo il client che è andato tutto bene spedendogli il messaggio da stampare	
		return -1;
	strcpy(parameter, filename);  	         // il chiamante troverà il nome del file nel 2° argomento, e lo stamperà a video (lato server)
	free(filepath);
	free(filename);   	          // libero la memoria dinamica utilizzata fin qui per path e nome del file inviato
	return 0; 	        	  // tutto ok se arrivo fin qui (la fine corretta di sSEND ritorna 0: file inviato)
} 
int sCOMPRESS ( int client_socket, char remote_path[], comp_param p, int PoolID, int* counter, char* client_IPaddr ) /* Corrispettivo client: cCOMPRESS.*/
{ /* ATTENZIONE: una volta creato tar i files inviati sono eliminati. [remote_path] è la directory dove il client vuole avere l'archivio compresso */
	int w, rc; 	 /*  la struct [p] contiene i parametri per la compressione; [PoolID] è l'id del serverthread chiamante; */         		    
	char *dyn; 	 /* [counter] contiene il n°  di files inviati fino ad adesso al server dal client con IPv4 [client_IPaddr]    */ 
	FILE *fp;                                                                     			   						 
	char archive_name[MAX_MSG_LEN+1];			   /*CREAZIONE ARCHIVIO TAR, INVIO AL CLIENT, ELIMINAZIONE*/			
	char archive_local_path[strlen(POOL_ROOT_DIR)+strlen(POOL_FOLDER_PREFIX)+MAX_MSG_LEN+10];     					
	char temp[ 20 + strlen(POOL_ROOT_DIR) + strlen(POOL_FOLDER_PREFIX) ];        
	unsigned int size;		        // usando un unsigned int per la dimensione del tar, questo potrà essere massimo 4 GiB
	struct stat inf;		 // conterrà in particolare il campo che mi dice quanto è grande il file compresso                                 
	strcpy(archive_name, p.archive_name);  						        // creo il nome dell'archivio compresso che verrà creato
	strcat(archive_name,".tar.");        							    // ..prima metto "tar"	   
	w = p.compressor_index;					           // ..poi l'estensione utilizzata dall'algoritmo di compressione in uso
	strcat(archive_name,compressors_matrix[w][1]);					 	// ..e concateno il tutto
	if ( !SendData(client_socket, archive_name, strlen(archive_name)) ) // 1) invio al client del nome dell'archivio compresso (NUL escluso)
		return -1; 
	strcat(remote_path, "/");    // creo il pathname della directory dove il client dovrà salvare l'archivio compresso che il server gli invierà
	if ( !SendData(client_socket, remote_path,  strlen(remote_path)) ) // 2)invio il pathname della directory dove il client deve salvare il tar inviato
		return -1; 	
	if ( ! ReceiveData(client_socket, &w, NULL) ) 		        	// 3) il path remoto è accessibile dal client (1) o no (0)?   
		return -1;	
	if (w==0) { 			       // se il client non può usare il percorso salta tutto (non può memorizzare localmente il tar che gli invierò 
		printf (REDf"Il client %s non puo' accedere al path %s."RST"\n", client_IPaddr, remote_path);
		return 1;
	}
	if ((*counter)==1) 						        	// nel tar ci saranno uno o più files?
		strcpy(temp,"file"); 
	else
		strcpy(temp,"files");
	printf("SERVER: compressione di "CYAf"%d"RST" %s in corso ", *counter, temp); // numero file che conterrà il tar
	printf("("CYAf"%s"RST"),", archive_name);							// nome dell'archivio
	printf("richiesta dal client "GREf"%s"RST".\n", client_IPaddr);		// indirizzo (IPv4) del client richiedente (a cui spedirò il tar)
	system( tar_cmd(p, PoolID) );     // comprimo (tar_cmd da' il comando aposito); non passo nomi di file (tutti quelli nella cartella del thread)	
	sprintf(archive_local_path, "./%s/%s%d/%s", POOL_ROOT_DIR, POOL_FOLDER_PREFIX, PoolID, archive_name); 
	w=1;							        	// suppongo che il tar sia stato creato e sia accessibile
	fp = fopen(archive_local_path, "rb");  		        	// apertura del file compresso (creato nella cartella locale dalla tar precedente) 
	if(fp==NULL)   
		w=0;    // errore apertura file tar
	rc = SendData(client_socket, &w, sizeof(int));    // 4) comunico al client se la creazione del file compresso è fallita (w=0) o è tutto ok (w=1) 
	if (w==0) {	
		fprintf (stderr, REDf"Impossibile creare o accedere al file archivio %s."RST"\n", archive_name);
		remove(archive_local_path); 			        	// gestione mancata apertura o creazione del file archivio 
		if (rc==0)
			return (-1);   	// il client s'è disconnesso 
		return 1;			  // il client è connesso e gli ho comunicato che non sono riuscito a creare o aprire l'archivio tar
	}
	stat(archive_local_path, &inf);// mi procuro la dimensione dell'archivio compresso appena creato (sta nella cartella locale del thread)
	size = inf.st_size;
	dyn = malloc(size);  // alloco un buffer dinamico grande quanto il tar creato
	fread(dyn, 1, size, fp);   // lettura del file(archivio tar) nella sua interezza e trasferimento del contenuto nel buffer          
	if ( ! SendData(client_socket, &size, sizeof(unsigned int)) ) // 5) invio dimensione tar: è vero che con la Send dopo invio di nuovo questo dato,>
		return -1;	        	// > ma il client prima di ricevere i dati deve sapere quanta memoria allocargli (può essere fino a 4GiB!)
	if ( ! SendData(client_socket, dyn, size) )     	// 6) invio del contenuto dell'archivio compresso (lo prelevo dal buffer)
		return -1;
	fflush(fp);							        	// pulizia stream (scrivo il rimanente sul file)
	fclose(fp);							        		// chiudo il file/tar locale	(fstream)
	free(dyn);						        	// libero il buffer dove avevo appoggiato il contenuto dell'archivio tar
	remove(archive_local_path);			       	// qualsiasi cosa sia accaduta comunque l'archivio non mi serve più (tanto ho i file...) 
	rc = ReceiveData(client_socket, &w, NULL);  // 7) ricevo l'esito della creazione dell'archivio, appena spedito,lato client: 0-errore, 1-tutto ok
	if ( (w==0) || (rc==0) )  {		        	// se il client non riesce a salvare (problemi sui file o perchè s'è disconnesso)
		printf (REDf"Il client non e' riuscito a salvare il file %s."RST"\n", archive_name);
		return 1;   				      // se il client non è riuscito a salvare l'archivio non devo cancellare i file finora inviati
	}
	(*counter) = 0;                                 	// tutto ok, per cui devo azzerare il computo dei file inviati da questo client e ...   
	sprintf(temp, "rm -r ./%s/%s%d", POOL_ROOT_DIR, POOL_FOLDER_PREFIX, PoolID);
	system(temp);                     	        	// ..cancellare la cartella "personale" del thread (contiene file inviati e archivio) .. 
	sprintf(temp, "mkdir ./%s/%s%d", POOL_ROOT_DIR, POOL_FOLDER_PREFIX, PoolID);
	system(temp);                     			// ..ed infine ricrearla vuota, per i prossimi invii.
	strcpy(remote_path, archive_name); 	        	// in questo modo comunico al chiamante il nome dell'archivio compresso
	return 0;
}

int sSHOWLIST ( int client_socket, int counter, int PoolID)  /* Corrispettivo sul client: cCMDS0_478{7: show-list}. */
{	     /* L'intero [counter] memorizza quanti   sono i file inviati finora dal client al [PoolID]-esimo thread, cui è stato assegnato   */
	char info[(counter+1)*MAX_MSG_LEN], temp[MAX_MSG_LEN];  	// messaggio e la lista dei nomi di tutti i file inviati fino ad adesso	
	struct dirent *de = NULL;                                           // per "scorrere" i file nella directory del thread
	if (counter==0) 													
		strcpy(info, CYAf"- Non sono stati ancora inviati file al server."RST"\n"); // se non ho file inviati mi fermo qui
	else {						        	// procedo a elencare i nomi dei file spediti dal client al suo serverthread
		char path[10+strlen(POOL_ROOT_DIR)+strlen(POOL_FOLDER_PREFIX)];	// deve contenere l'intero percorso della cartella locale di questo thread
		DIR* d;
		sprintf(path, "./%s/%s%d/", POOL_ROOT_DIR, POOL_FOLDER_PREFIX, PoolID);	  
		d = opendir(path);			       	// apro la directory locale di questo thread per poter vedere i files contenuti		
		if (counter==1) 				        	// se c'è un solo file il ciclo sarà di 3 ma una sola stampa video 
			sprintf(info, CYAf" - Il server ha ricevuto il seguente file:\n");
		else  
			sprintf(info, CYAf" - Il server ha ricevuto i seguenti "GREf"%d"CYAf" files:\n", counter);
		while ( (de = readdir(d))!= NULL ) {	        	// passo in rassegna tutti i file contenuti nella cartella locale
			if ( strcmp(de->d_name,".")!=0 && strcmp(de->d_name,"..")!=0) {	// escludo la DIR corrente e quella padre	
				sprintf(temp,"%4c-> %s\n",' ', de->d_name);	// "%4c" significa che va inserito 4 volte il char specificato (lo spazio)
				strcat(info, temp);     		// preparo la lista dei nomi (non i path!) dei files, esclusi "." e ".." 
			}
		}
		closedir(d);		        	// ho finito di scorrere la cartella, quindi chiudo la directory
	}
	return ( SendData(client_socket, &info, strlen(info)) -1 ); 		// 1) invio messaggio, con gestione errori inclusa		
}

int sEMPTYLIST ( int client_socket, int counter, int PoolID ) /* Corrispettivo sul client: cCMDS00_478{8:empty-list}. */
{		         /* [counter] e' il n° di file inviati finora dal client al [PoolID]-esimo threadserver, al quale è stato assegnato   */
	char temp[MAX_MSG_LEN];	        	// vi appoggio il comando da eseguire e poi il messaggio sull'esito (da mandare al client)
	if (counter>0) {			        				// se c'è almeno un file inviatomi dal client
		sprintf(temp, "cd ./%s/%s%d/ && rm * && cd .. && cd ..", POOL_ROOT_DIR, POOL_FOLDER_PREFIX, PoolID);
		system(temp);   // entro nella cartella locale del thread, cancello tutto e poi "torno su" dove gira il thread
	}       // non decremento il contatore (oltretutto passato per valore), ci penserà il chiamante
	strcpy(temp, CYAf" - Sono stati eliminati tutti i file che erano stati inviati al server.\n"RST);				
	return ( SendData(client_socket, &temp, strlen(temp)) -1 ); 	// 1) invio messaggio con gestione errori inclusa nella funzione chiamata
}



/* GESTORI DI SEGNALI */

void gestoreSIGINT ( int signum )  /* Gestore del segnale SIGINT(2).*/
{		/* [signum] sarà sempre 2, poiché ridefinisco solo la INT, in modo che Ctrl+C provochi la chiusura ordinata del server */	
	signal(SIGINT, gestoreSIGINT);  		// per retrocompatibilità con alcuni vecchi sistemi operativi
	pthread_mutex_lock(&mutex);				// poiché accedo a in_service, closing, PoolSleep,
	if (in_service>0)  	        	// il segnale non ha effetto:  il programma si può chiudere solo quando tutti i client si sono disconnessi
		printf(REDf"\nNon e' possibile terminare il programma finche' ci sono client connessi!"RST"\n");
	else { 		    // SIGINT fa partire la procedura di chiusura del programma; nell'ordine: pool thread, main thread (via join), main (via join)
		closing=1;  						// settaggio che indica globalmente l'inizio della chiusura ordinata del server
		printf(YELf"\nRicevuto segnale INT: avvio procedura di terminazione del server."RST"\n");
		pthread_cond_broadcast(&PoolSleep); // sveglio i pool thread, che sono certamente tutti inattivi, poichè devono terminare
		shutdown(ss, 2);    // chiudo il list. socket, così sblocco il thread  sulla accept, in modo che possa terminare (e dopo di lui il main)
	}
	pthread_mutex_unlock(&mutex);			// avendo fatto la lock all'inizio
}
	
	

/* CODICI DEI THREAD SERVER */

// thread del pool
void *codice__Server_Thread ( void *PoolID ) /* THREAD SERVER: codice di ciascuno dei POOL_DIMENSION ServerThread del pool, creato dal ListenerThread */
{ 	
	int id, file_counter, choiceID, Bs_rcvd, c_sock, rc;    	// c_sock è il socket con il quale il thread comunica col client assegnatogli 
	char clientCommand[MAX_MSG_LEN+1], parameters[MAX_MSG_LEN+1];
	char shellCommand[ 20 + strlen(POOL_ROOT_DIR) + strlen(POOL_FOLDER_PREFIX)];
 	int quit;        //segnala se la disconnessione del client è stata anomala (0) o no (1, cioè tramite il comando quit)
 	struct sockaddr_in c_address;   	// ci metterò i dati del client servito (passati dal Listener tramite la variabile globale c_sockaddr_array)
 	comp_param p;   		// la struct contiene i parametri (nome compressore e archivio) per questo thread (messi di default ad ogni nuovo >
	p.archive_name=NULL;		    // > client): inizialmente tale struttura è vuota (pointer a NULL e indice inesistente) ma sarà presto riempita
	p.compressor_index=-1;
	id = *(int*)PoolID;		   // id assegnato dal padre al thread in esecuzione (da 0 a POOL_DIMENSION-1), non è il suo TID (quello di self)!!
	printf(RST"Creato thread %d.\n", id);           // informo che sono stato creato
	pthread_mutex_lock(&mutex);	        	// poichè accedo alla variabile globale ReadyThreads e poi uso la signal
	if ( (++ReadyThreads)==POOL_DIMENSION )  // se è l'ultimo PoolThread a bloccarsi sveglia il Listener (in attesa sulla create_pool) in modo che >
		pthread_cond_signal(&PoolReady); // > esso sappia che tutti i thread del pool sono pronti e può iniziare fare le accept e assegnare i client
	pthread_mutex_unlock(&mutex);	        	// provvede eventualmente anche a rilasciare il lock per la signal
	do {     	        	// ad ogni ciclo ci si blocca in attesa che gli si assegni un client e quando si sveglia lo si serve  
		sprintf(shellCommand, "mkdir %s/%s%d", POOL_ROOT_DIR, POOL_FOLDER_PREFIX, id); 
		system(shellCommand);	        	// creo la cartella personale del thread (sotto POOL_ROOT_DIR, già creata dal ListenerThread)
		file_counter = 0;    		        	// inizialmente non ho file inviati dal client
		quit=0;			      //solo il comando quit lo setta, segnalando una disconnessione del client ordinata 		
 		if (p.archive_name!=NULL)    // se puntava ad un vecchio valore (il nome scelto dall'ultimo client servito) elimino la stringa (dinamica) >
			free(p.archive_name); // >  puntata dal campo p della struttura, in modo che successivamente possa ricrearla col nome di default    
 		p.archive_name = malloc( (strlen(DEFAULT_ARCHIVE_NAME)+1)*(sizeof(char)) );
		strcpy ( p.archive_name, DEFAULT_ARCHIVE_NAME );                // impostazione di default sul nome dell'archivio compresso (una stringa)
 		p.compressor_index = DEFAULT_COMPRESSOR_INDEX; // opzione di default sul compressore da utilizzare (indice entry compressors_matrix)
		wait_and_start(&c_sock, &c_address, id);       // attendo che il main thread mi assegni un client oppure mi svegli la SIGINT per terminare 
		while(closing==0){ // resta in attesa di comandi: una volta entrato nel  ciclo interagisce col client assegnatogli (finisce con INT)	 
			char* clientIP = inet_ntoa(c_address.sin_addr);       // traduco in una stringa l'indirizzo IP del processo client che sto servendo
			if ( ! ReceiveData(c_sock, &clientCommand, &Bs_rcvd) )  	// 2) ricezione del comando inviato dal client
				break;  	// se il client salta termino il ciclo di attesa comandi e avvio la procedura per ricevere un altro client
			clientCommand[Bs_rcvd] = '\0';
			choiceID = identify_command(clientCommand, parameters); // analisi del comando e individuazione eventuale/i parametro/i dello stesso
			if ( ! SendData(c_sock, &choiceID, sizeof(int)) ) 		 // 3) invio al client il numero d'ordine del comando ricevuto 
				break;	      // se il client salta termino l' attesa comandi e avvio la procedura per ricevere un altro client
			switch(choiceID){       	// a seconda del comando ricevuto (suo n° d'ordine) faccio determinate azioni  
				case 0:{ //COMANDO NON VALIDO
						if (sINVALIDCOMMAND(c_sock)==-1)  // se ho problemi con il socket passo a servire un altro client
							break;
						continue;	        // semplicemente il comando non è valido quindi ne chiedo un altro a prompt
				}
				case 1:{ //help
						if (sHELP(c_sock)==-1)   // se ho problemi con il socket servo un altro client (questo s'è disconnesso)
							break;
						printf(YELf"CLIENT "CYAf"%s"YELf" eseguito il comando "
								GREf"help"YELf".\n"RST, clientIP);		// esito positivo
						continue;      // sHelp restituisce o 0 o, se arriva qua, 1, cioè è andato tutto bene			
				}
				case 2:{ //configure-compressor [name]
						int ris = sCONFIGURECOMPRESSOR(c_sock, parameters, &p);
						if (ris==-1)    // gestione errore di comunicazione col client via socket: ne servo un altro
							break;
						if (ris==0)          // la configurazione del compressore era corretta, quindi il comando è stato eseguito 
							printf(YELf"CLIENT "CYAf"%s"YELf" eseguito il comando "
									GREf"%s"YELf".\n"RST, clientIP, clientCommand); // esito positivo 
						continue;  // passa al ciclo dopo sia se il compressore indicato esisteva (ris==0) sia se no (ris==1)
				}
				case 3:{ //configure-name [name]
						int ris = sCONFIGURENAME(c_sock, del_chars(parameters,'\"'), &p);
						if (ris==-1) 
							break;	// gestione errore di comunicazione su socket: mi rendo libero per un altro client
						if (ris==0) // tutto ok: il nome dell futuro archivio è stato cambiato: il comando ha avuto successo	
							printf(YELf"CLIENT "CYAf"%s"YELf" eseguito il comando "
							       GREf"%s"YELf".\n"RST, clientIP, clientCommand);	// esito positivo	
						continue;    // torno al prompt sia se il nome andava bene sia se era "vuoto" (tutti spazi)	
				}
				case 4:{ //show-configuration
						if (sSHOWCONFIGURATION(c_sock,  &p)==-1)
							break;	       	// fallisce solo se cade la connessione: in tal caso mi libero per un altro client
						printf(YELf"CLIENT "CYAf"%s"YELf" eseguito il comando "
								GREf"show-configuration"YELf".\n"RST, clientIP ); // esito positivo
						continue;	
				}
				case 5:{ //send [file]
					char temp[MAX_MSG_LEN];
					int i, counter;
					list FilesToSend = create_path_list(parameters, &counter); // lista dei file che il client vuole inviarmi
					if ( ! SendData(c_sock, &counter, sizeof(int)) ) 	// 0) invio al client il n° dei file che mi deve spedire 
						break;					
					for(i=0; i<counter; i++) {  // finchè ci sono file da inviare
						strcpy(temp, extract_path(&FilesToSend));  //  estraggo dalla testa il path del file da inviare e lo salvo
						rc = sSEND(c_sock, temp, id, &file_counter);
						if ( rc == -1)   	// il client non risponde, mi libero per poter essere assegnato ad un altro
							break;
						if ( rc==1 )      // file non inviato per problemi non critici (e.g. path inesistente, permessi mancanti)..
							continue; // ..passo a quello successivo
						strcpy(parameters,temp);
						if (file_counter==1) 	// invio tutto ok
							strcpy(temp,"("CYAf"1"RST" file ricevuto).\n");
						else    	// preparo il messaggio di successo per l'invio di questo singolo file
							sprintf(temp,"("CYAf"%d"RST" file ricevuti).\n", file_counter);
						printf("SERVER: ricevuto il file "CYAf"%s"RST" dal client "
								GREf"%s"RST" %s", parameters, clientIP, temp); // esito positivo
					}
					continue;
				}
				case 6:{ //compress [path]
					if ( ! SendData(c_sock, &file_counter, sizeof(int)) ) // 0) deduce da countere quello cosa fare (nulla se e' 0)    
						break;     // problema di connessione: torno all'assegnazione di un nuovo client da parte del ListenerThread
					if (file_counter!=0) {  	//  solo se sono stati inviati file faccio partire la funzione di decompressione
						rc = sCOMPRESS(c_sock, parameters, p, id, &file_counter, clientIP ); 
						if (rc == -1)   	// c'è stata la disconnessione del client durante l'esecuzione della sCompress 
							break;
						if (rc==0) 										// tutto bene
							printf("SERVER: spedito archivio compresso "CYAf"%s"RST" al client "
									GREf"%s"RST".\n", parameters, clientIP );	
					}      	// il caso di rc=1 significa che la compress ha avuto problemi e quindi non è stata eseguita tutta e >
					continue;      	// dunque come nel caso di successo vado semplicemente a ricevere un nuovo comando dal prompt
				}
				case 7:{ //show-list
						if (sSHOWLIST(c_sock, file_counter, id)==-1)  // problemi col s. del client? Mi libero per servirne un altro
							break;
						printf(YELf"CLIENT "CYAf"%s"YELf" eseguito il comando "
								GREf"show-list"YELf".\n"RST, clientIP );
						continue;       	// questa funzione non ha successo solo se salta la connessione	
				}
				case 8:{ //empty-list
						
						if (sEMPTYLIST(c_sock, file_counter, id)==-1) // problemi socket del client? Mi libero per servirne un altro
							break;
						file_counter=0;
						printf(YELf"CLIENT "CYAf"%s"YELf" eseguito il comando "
								GREf"empty-list"YELf".\n"RST, clientIP ); // esito positivo
						continue;       	// questa funzione non ha successo solo se salta la connessione	
				}			
				case 9:{ //quit
					quit=1;				// la disconnessione del client avviene in modo corretto
					break;          	// esco dallo switch (voglio liberarmi e servire un nuovo client)
				} 
			}    	//fine switch
			break;         	// se esco dallo switch (solo via quit) esco anche dal while sul prompt (attesa comandi) 		
		} 	// fine while sul prompt (mancato contatto col socket del client o quit dello stesso)
		if (closing==0) {       // se è vero sono uscito per disconnessione del client, non per arrivo della SIGINT (chiusura server) 
			pthread_mutex_lock(&mutex);    	// decremento  in_service (devo usare il mutex) per iniziare la procedura di liberazione..
			in_service--;               // .. ma non sveglio subito il Listener (lo farò nella  wait_and_start) altrimenti (se tutti i thread >
			pthread_mutex_unlock(&mutex);  // >  sono occupati) potrebbe accadergli di fare assign_client, trovare in_service<NUMTHREADS ed   > 
			if (quit==1){ //disconnessione client via quit
				if (shutdown(c_sock, SHUT_RDWR)<0)      	// >  eseguire una signal senza effetto perchè questo thread non è ancora pronto
					perror("shutdown");
				if (close(c_sock)<0)         								
					perror("close");	 	// chiudo il socket di comunicazione ("connected") col client che stavo servendo 
				printf( REDf"CLIENT "RST"%s"REDf" chiude la connessione ", inet_ntoa(c_address.sin_addr) );
			}
			else { 							// la connessione col client è saltata (non per effetto del comando quit)
				shutdown(c_sock, SHUT_RDWR);
				close(c_sock);
				printf( REDf"CLIENT "RST"%s"REDf" disconnesso in modo inaspettato ",inet_ntoa(c_address.sin_addr) );	
			}			
			printf( "["RST"%d"REDf"/%d thread liberi]\n"RST, (POOL_DIMENSION-in_service), POOL_DIMENSION ); 
		} 
		sprintf(shellCommand, "rm -r ./%s/%s%d", POOL_ROOT_DIR, POOL_FOLDER_PREFIX, id); 
		system(shellCommand);   // cancello la cartella personale del pool thread server (la directory madre verrà eliminata dal Listener)
	} 	// fine while del pool thread server (vi esco solo se il server sta terminando)
	while (closing==0);     	// condizione del do..while in cui ad ogni ciclo servo un client
	printf( RST"\nTerminato thread %d", id );
	pthread_exit(NULL);
} //fine codice pool thread 


// thread di ascolto
void *codice__Listener_Thread ( void* serverPort ) /* THREAD LISTENER: creato main, a sua volta crea POOL_DIMENSION thread e si mette in ascolto  > */
{  /*> di richieste di client da assegnare loro; [serverPort] indica la porta su cui ascolta il server (necessario per creare il socket d'ascolto)     */
	int i, rc, listening_sock_server, port;             // il socket è di tipo "listening" (per accettare le richieste)
	struct sockaddr_in client_address, server_address;  // contengono l'indirizzo del client e del server
	pthread_attr_t attr;
	pthread_t pool_thread[POOL_DIMENSION];
	int *taskids[POOL_DIMENSION];
	int saddrlen= sizeof(struct sockaddr_in); 		    // lunghezza struttura sockaddr_in 
	int option = 1;				         // per settare il SO_REUSEADDR della listening socket a "true" (non-zero value)
	char shellCommand[50+2*strlen(POOL_ROOT_DIR)];
	void *status=NULL;				      	// per la join sui thread del pool quando sto terminando
	printf(GREf"Creato thread di ascolto."RST"\n");     // informo che sono stato creato  
	sprintf(shellCommand, "rm -fr %s && mkdir %s", POOL_ROOT_DIR,  POOL_ROOT_DIR);        
	system(shellCommand);			        	// directory che conterrà le POOL_DIMENSION cartelle dei vari Thread del pool 
	pthread_attr_init(&attr);                           // inizializzazione attributi
	pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_JOINABLE);
	create_pool( taskids, pool_thread, codice__Server_Thread ); // creazione pool (POOL_DIMENSION thread gestori)
	port = *(int*) serverPort;
	memset(&server_address, 0, sizeof(server_address)); // preparazione indirizzo del server (in ascolto su una qualsiasi delle sue NIC)
	server_address.sin_family = AF_INET;                // uso le HtoNl/s perchè l’indirizzo IP (Long) ed il n° di porta (Short)  devono essere > 
	server_address.sin_addr.s_addr = htonl(INADDR_ANY); // > specificati nel formato di rete (Network order, big endian) in modo da essere      >
	server_address.sin_port = htons(port);              // > indipendenti dal formato usato dal calcolatore (Host order).
	listening_sock_server = socket(PF_INET, SOCK_STREAM, 0); // creazione del socket di ascolto
	ss = listening_sock_server;     	// vedendo tale socket SIGINT potrà sbloccare il Listener bloccato con la accept() su esso
	if (listening_sock_server==-1){      
		char *sret = malloc(15);
		strcpy(sret,"Socket error");
		ListenerSock_and_Sem_Destroy(listening_sock_server);
		perror("socket");   							// errore nella creazione del socket di ascolto
		pthread_exit((void*)sret);
	}
	if (setsockopt(listening_sock_server, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option))<0){ 
		char *sret = malloc(20); // nel caso di restart/crash del server cerco di evitare l'"address already in use" sulla bind
		strcpy(sret,"Setsockopt error");		
		ListenerSock_and_Sem_Destroy(listening_sock_server);
		perror("sockopt");								
		pthread_exit((void*)sret);
		return 0;
	} 
	if (bind(listening_sock_server, (struct sockaddr*)&server_address, saddrlen)<0){ // binding (indirizzo#porta su cui il server accetta le richieste)
		char *sret = malloc(15);
		strcpy(sret,"Bind error");	
		ListenerSock_and_Sem_Destroy(listening_sock_server);
		perror("bind");									
		pthread_exit((void*)sret);	
	} 
	if (listen(listening_sock_server, BACKLOG)<0) {     // mi metto in ascolto delle connessioni in ingresso sull'apposito socket       
		char *sret = malloc(15);
		strcpy(sret,"Listen error");		
		ListenerSock_and_Sem_Destroy(listening_sock_server);
		perror("listen");
		pthread_exit((void*)sret);
	}
	printf (GREf"Attesa di connessioni..."RST"\n"); 
	while(1) {           		      	// rimane permanentemente in attesa di connessioni richieste dai client, poi le smista ad un thread del pool
		int client_sock; 	       	// socket di tipo "connected" (per la comunicazione vera e propria con il client)
		socklen_t len = sizeof(struct sockaddr_in);                       
		client_sock = accept(listening_sock_server, (struct sockaddr*)(&client_address), &len); //estrae richiesta su listening s., crea connected s.
		if (client_sock<0) {   printf("sock=%d\n",client_sock);
			if (closing==0)
				perror("accept");        // è "giusto" che la accept dia errore quando SIGINT chiude il listening socket (closing=1)
			break;
		}
		if (closing==0)
			assign_client(client_address, client_sock); // sveglio dal pool un Thread Server a cui assegnare il client che si sta connettendo
		else
			break;          	// quando termino esco dal ciclo (non c'è un client connesso poichè la accept è stata terminata da SIGINT)  
		if ( ! SendData(client_sock, &i, sizeof(int)) )	// 1) comunico al client che gli ho assegnato un thread del pool (potrà inviare comandi)
			continue;	 	// se il client che si era connesso non riceve nulla passo al prossimo
	} // per effetto della SIGINT il ciclo termina (break del secondo if dentro il ciclo infinito)
	for (i=0; i<POOL_DIMENSION; i++) { // da qui si passa al codice del singolo thread
		rc = pthread_join(pool_thread[i], (void**)&status); // si blocca finchè non terminano tutti i pool threads (figli), poi > 
		if (rc) {		        	            // > quando gli ha joinati tutti (sono finiti) prosegue la sua procedura di chiusura
			char *sret = malloc(15);
			printf("Errore nel join del thread n° %d: codice di ritorno %d\n,", i, rc);
			strcpy(sret,"Join error");		
			ListenerSock_and_Sem_Destroy(listening_sock_server);
			perror("join");
			pthread_exit((void*)sret);
		}	    
	} // fine attesa di join su tutti i thread del pool
	ListenerSock_and_Sem_Destroy(listening_sock_server);// distruggo i semafori e chiudo il socket di ascolto 
	pthread_attr_destroy(&attr); 						
	sprintf(shellCommand, "rm -r %s", POOL_ROOT_DIR);// distruzone directory "madre", quella che conteneva le cartelle dei thread, ciascuno >
	system(shellCommand);			                   // > dei quali ha già provveduto, prima di terminare, a cancellare la propria (e.g. xx22)	
	printf(GREf"\nTerminato thread di ascolto."RST"\n");  // comunico che il listener thread è in procino di terminare (sarà jionato dal main)
	pthread_exit(NULL);
} //fine main thread



/*       CORPO DEL PROCESSO SERVER     */

// main (compressor-server)
int main ( int argc, char* argv[] ) /* Il processo server si limita ad alcune azioni base e poi delega  il servizio al ListenerThread (che a > */
{  			            /* > sua volta lo smisterà tra i ServerThreads del pool); la sintassi è "compressor-server <porta>"        */
	pthread_t main_thread;     
	pthread_attr_t attr;                    // per il thread listener
	int port, rc; 
	void *status=NULL;	        	// per la join sul ListenerThread
	struct sigaction sa;
	sa.sa_handler = gestoreSIGINT;               //assegno il signal handler per la SIGINT(ctrl+c)
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGINT, &sa, NULL) == -1) 
	  fprintf (stderr,"Errore inizializzazione handler SIGINT via sigaction\n\n");						
	signal(SIGINT, gestoreSIGINT);	
	assignedFlag=1;      // inizialmente non ci sono client in attesa quindi nessuno di essi aspetta un thread
	closing=0;	     // inizialmente la procedura di chiusura del server (via INT) è disattivata
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_JOINABLE);    // inizializzazione del mutex e degli attributi del main thread
	pthread_mutex_init(&mutex, NULL); 								
	if (argc!=2) {   			      // gestione errori sul n° dei parametri con cui viene lanciato il server 
		fprintf (stderr, REDf"\nIl programma compressor-server deve essere lanciato specificando "
				       "la porta su cui si deve mettere in ascolto il server."RST"\n\n");
		return 0;
	}
	port = atoi(argv[1]);              												
	if ( (port<1024)||(port>65535) ) {    	// intervallo di porte ammesse
		fprintf (stderr, REDf"\nNumero porta non valido (intero compreso tra 1024 e 65535)."RST"\n\n");
		return 0;								        	// gestione errore sul numero di porta
	}
	printf (YELf"\nProcesso server (pid "RST"%d"YELf            // non usando una well-known port il client dovra' conoscere su quale il server ascolta
	              ") in ascolto sulla Porta "CYAf"%d"YELf"."RST"\n\n",getpid(),port);     //se si vuole usare kill per arrestare il server
	printf (REDb"REMOTE COMPRESSOR server, v %s"RST"\n", VERSION);          // comunico l'avvio del processo server
	if (pthread_create(&main_thread, &attr, codice__Listener_Thread, &port)<0) {   //  creazione Thread Listener: uso un thread perchè quando (ad es.) >
	       fprintf (stderr, REDf"Errore di creazione del main thread."RST"\n"RST); //  > il pool è tutto occupato il main si deve bloccare per    >
	       exit(-1);                                                               //  > poi essere svegliato: essendo più leggero conviene       >
	}                                                                              //  > bloccare e svegliare un thread piuttosto che un processo >
	rc = pthread_join(main_thread, (void**)&status);      // attesa terminazione del thread creato (quando arriva SIGINT al processo server)
	if (rc) { 
		printf(REDf"Errore nel join: codice di ritorno %d"RST"\n", rc);	// gestione errore sul join
		exit(-1);
	} 		// attendo che il Thread Listener termini (significa che sono terminati tutti i pool thread perchè lui li joinava prima di uscire)
	if (status!=NULL)	{						//gestione uscita anomala in uno dei thread del pool
		char *msg=status; 		
		printf("%s nel Server Thread principale:", msg);
		free(status);
	}
	pthread_attr_destroy(&attr);  
	printf(REDb"Terminazione REMOTE COMPRESSOR server."RST"\n\n"); 		// il processo compressor-server sta per terminare
	return 0;				       // la pthread_exit servirebbe se morto il main altri thrtead andassero avanti, ma li ho tutti joinati
} // fine codice del processo main