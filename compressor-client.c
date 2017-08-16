/*
 * author: Andrea Orlandi
 * name: compressor-client.c
 * date: 28/02/2017
 * description: lato client per il progetto di programmazione distribuita del corso universitario di 
 * 				Organizzazione dei Sistemi Operativi e Reti (OSOR), CdL Triennale in Ingegneria 
 * 				Informatica dell'Università di Pisa, tenuto dal prof. Anastasi.
 * 				Le caratteristiche principali dell'applicazione "remote-compressor" sono:
 * 					- paradigma client-server
 * 					- server concorrente multi-threaded (thread POSIX)
 * 					- comunicazione tramite Berkeley socket TCP ("stream")
 * 				   - compressione tramite il comando "tar"
 * 					- utilizzo dei segnali (ISO C library signals)
 * 					- utilizzo delle espressioni regolari (POSIX ERE)
 * language: Italian (program, comments), English (code)
 * notes: 1) programma scritto per l'esecuzione sotto ambienti UNIX e *nix
 * 	  2) i client devono conoscere indirizzo (IPv4) e porta sul quale sta in ascolto il server
 *        3) massima dimensione dei file inviabili: 4GiB
 * launch: compressor-client <host-remoto> <porta>          
*/

/*  STRUTTURA DEL DOCUMENTO: 
		- librerie (base, socket)
		- macro (messaggi, versione, colori)
		- typedef (archiviazione, lista di nomi)
		- funzioni (stringhe, socket, funzioni del client)
		- codice processo (compressorclient)
*/


/*      LIBRERIE    */
#include <stdio.h>      // librerie base
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>  // librerie socket
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*  MACRO  */
#define MAX_MSG_LEN 200  /* dimensione massima dei messaggi che può inviare il client */

#define VERSION "6.3" /* versione del programma */

#define PROMPT "remote-compressor> " /* command prompt a schermo */

#define CYAf   "\x1B[36m"    /* colori */
#define GREf   "\x1B[32m"         // testo
#define REDf   "\x1B[31m" 
#define WHIf   "\x1B[37m"
#define YELf   "\x1B[33m"
#define MAGb   "\x1B[45m"         // sfondo
#define REDb   "\x1B[41m" 
#define RST    "\033[0m"	    	 // reset	


/* FUNZIONI */ 

// funzioni (2) sulle stringhe [uguali per client e server]
char *trim_side_spaces ( char *s )  /* toglie gli spazi a sx e dx della stringa [s] e la restituisce (modificata) */	
{ 
  int i=0, k=0, j=(strlen(s)-1);
  while ( isspace(s[i]) && s[i]!='\0' )
    i++;
  while ( isspace(s[j]) && j>=0 )
    j--;
  while ( i<=j )
    s[k++]=s[i++];
  s[k]='\0';
  return s;
}

char *del_chars ( char *r, char x )  /* toglie tutte le occorrenze del carattere [x] nella stringa [r] e la restituisce (modificata) */	
{
  int i=0, k=0;
  char s[strlen(r)+1];
  strcpy(s,r);
  for ( i=0; s[i]!='\0'; i++ ) 
		if (s[i]!=x)
			r[k++]=s[i];
  r[k]='\0';
  return r;
}

// funzioni (2) sui socket: 1-ok, 0-errore 
   /* sono duali: quando c'è una dall'altra parte della connessione c'è l'altra: esse fanno tx dimensione dati-> rx dimensione dati -> tx dati -> rx dati */
int SendData (int sock, const void *data, size_t dim) /* va avanti finché non invia il blocco, grande [dim], puntato da [data] al socket [sock] */
{
    int total = 0, rc, n, bytesleft = dim;
    rc = send( sock, &dim, sizeof(int), 0 );        // invio dimensione (ReceiveData deve sapere quanti bit aspettarsi)
    if ( (rc==-1)||(rc<sizeof(int)) )
		       return 0;					
    while(total < dim) {						 // ciclo finché non ho inviati "dim" dati oppure c'è un errore)
        n = send( sock, data+total, bytesleft, 0 );
        if (n == -1) 
		  break;
        total += n;
        bytesleft -= n; 
    }
    return (n==-1)?0:1; 						 // comunico l'esito al chiamante
} 

int ReceiveData (int sock, void *data, int *len) /* va avanti a ricevere dati da [sock], memorizzando i bit dove punta [data] e il n° dove punta [len] */
{
    int total = 0, rc, n, dim, bytesleft;         
    rc = recv( sock, &dim, sizeof(int), 0 ); 			// SendData mi dice quanti dati mi invierà 	
    if ( (rc==-1)||(rc<sizeof(int)) )			 
	 	     return 0;			       	// errore su ricezione quantità di dati in arrivo
    bytesleft = dim;
    while(total < dim) {
        n = recv( sock, data+total, bytesleft, 0 );     
        if (n == -1) 			       	// errore su ricezione dati
		break;
        total += n;
        bytesleft -= n;
    } 
    if (len!=NULL)		       // se SendData (corrispettivo) voleva sapere quanti dati mi sono arrivati faceva puntare >
	       *len=dim;               // > [len] a una locazione (un intero per la precisione): in tal caso ci metto la quantità richiesta 
    return (n==-1)?0:1; 
} 

// funzioni (3) eseguite dal client quando richiede un servizio tramite un comando

void cCMDS0_478 (int sock_client) /* help(1),show-config(2),config-name(3),config-compressor(4),show-list(7),empty-list(8), caso di comando non valido (0)*/
{
	char msg[MAX_MSG_LEN*2] = "";					
	int Bs_rcvd;
	if ( ! ReceiveData (sock_client, &msg, &Bs_rcvd) ){ // 1) ricevo e stampo il messaggio che arriva dal server
	  	fprintf (stderr, "Impossibile comunicare col server\n");
		return;                 // errore nella comunicazione col compressor-server
	}
	msg[Bs_rcvd]='\0'; 	         // append di NUL al vettore di caratteri ricevuto, che non lo comprendeva (risparmio 1B)
	printf("%s", msg);
}

void cSEND (int sock_client)      /* Invio al server di un singolo file (corrispettivo sul server: "sSEND") */
{
	FILE *fp;      	// per operare sul file da inviare
	struct stat inf;            // vi metterò la lunghezza del file da inviare
	unsigned int size; 				       // usando un intero senza segno per la dimensione del tar esso potrà essere al massimo 4Gib 
	int risp, Bs_rcvd; 
	char *dyn;   						       	// punterà alla zona di memoria dove metterò temporaneamente il file da inviare
	char filepath[MAX_MSG_LEN], msg[MAX_MSG_LEN]; 
	if ( ! ReceiveData (sock_client, &filepath, &Bs_rcvd) )   		// 1)ricevo dal server il percorso del file da inviare	
		return;		
	filepath[Bs_rcvd]='\0';			
	stat( filepath, &inf ); 	// informazioni varie sul file
	if ( S_ISREG(inf.st_mode)==0 || access(filepath,R_OK)==(-1) ) { // gestione problemi d'accesso al file da inviare
		fprintf (stderr, REDf"- "MAGb WHIf"%s"RST REDf": percorso non corrispondente ad un file accessibile in lettura."RST"\n", filepath);
		risp=0;
		if ( !SendData(sock_client, &risp, sizeof(int)) )   // 2e) comunico al server che l'invio file è fallito perchè il file non esiste (mando 0)
			return;		
		return;	
	}
	risp=1; // controlli sul path del file da inviare andati a buon fine
	if ( ! SendData(sock_client, &risp, sizeof(int)) )	 // 2) comunico al server che è tutto ok e dunque mi appresto ad inviare il file (mando 1) 
		return;		
	if ( ! ReceiveData(sock_client, &risp, NULL) )           // 3) il server mi dice come proseguire: "-1"-tutto ok. "0"-file già inviato 
		return;		
	if (risp==0) {
		fprintf (stderr, REDf"- %s: al server e' stato gia' inviato un file con questo nome."RST"\n", filepath);  
		return;
	}	
	size = inf.st_size;    	       	// mi procuro la dimensione del file da inviare
	if ( ! SendData(sock_client, &size, sizeof(unsigned int)) ) 	// 4) invio dimensione file
		return;		
	if (size!=0) {             //se sto mandando un file vuoto non devo
		risp=1;					// ipotizzo che l'apertura del file abbia successo (d'altronde ho controllato già l'accesso)
		fp = fopen(filepath, "rb");   		// apro il file da inviare (in lettura perchè devo solo mandare il suo contenuto al server)
		if (fp==NULL) {
			risp=0;
			if ( ! SendData(sock_client, &risp, sizeof(int)) )  // 5e[opz]) comunico al server non riesco a aprire il file:  la send termina
				return;
			fprintf (stderr, REDf"- %s: impossibile aprire il file."RST"\n", filepath);
			return;
		}
		if ( ! SendData(sock_client, &risp, sizeof(int)) )		 // 5[opz]) comunico al server che l'apertura del file da inviare è riuscita
				return;
		dyn = malloc(size);   			       	// alloco memoria dinamica per i dati del file aperto, che userò poi per mandarlo via socket
		fread(dyn, 1, size, fp);        	// lettura file	
		if ( ! SendData(sock_client, dyn, size) )  	         // 6[opzionale se file nn vuoto]) invio del contenuto del file
			return;	
		fflush(fp);	 	// un po' di pulizia..
		fclose(fp);   
		free(dyn); 
	}
	if ( ! ReceiveData (sock_client, &msg, &Bs_rcvd) )  		    // 7) ricevo dal server il messaggio (win or fail) da visualizzare
		return;		
	msg[Bs_rcvd]= '\0'; 
	printf(CYAf"%s"RST, msg);   								// stampo a video il messaggio ricevuto dal server
}

void cCOMPRESS (int sock_client)  /* Compressione remota di uno o più file e ricezione dell'archivio così creato (corrispettivo sul server: "sCOMPRESS") */
{					        // ATTENZIONE: una volta creato l'archivio compresso i file inviati vengono eliminati
	FILE *fp;			        // per salvare il tar inviatomi  								  
	int y, risp, Bs_rcvd;
	unsigned int size;        		           // poichè uso un intero C senza segno per la dimensione dell'archivio compresso, questo >
	char *tmp_archive;        		           // > potrà essere grande al massimo 4.294.967.295 B (4GiB), ipotesi fattibile 
	char temp[MAX_MSG_LEN]="", path[MAX_MSG_LEN*2]="";
	struct stat sb;	
	if ( ! ReceiveData (sock_client, &y, NULL) )   		 // 0)  y>0: ci sono file inviati, y=0: non sono stati inviati file */
		return;		
	if (y==0) {
		printf (REDf"- Al server non e' stato inviato alcun file.\n"RST); // da qui in poi il suo corrispettivo sul server è sCOMPRESS
		return;
	}
	if ( ! ReceiveData (sock_client, &temp, &Bs_rcvd) )  // 1) ricevo dal server il nome dell'archivio compresso e lo memorizzo in temp*/        
		return;		
	temp[Bs_rcvd]='\0'; 								 // contiene il nome dell'archivio (e.g."nome.tar.xz")
	if ( ! ReceiveData (sock_client, &path, &Bs_rcvd) )  // 2) ricevo dal server il percorso dove salvare l'archivio compresso e lo memorizzo*/
		return;		
	path[Bs_rcvd]='\0'; 					         // contiene il path della directory dove salvare l'archivio(e.g."./alfa/beta/")
	risp=1;
	if ( stat(del_chars(path,'\"'), &sb)!=0 || S_ISDIR(sb.st_mode)==0 || access(del_chars(path,'\"'), W_OK)!=0 ) 
		risp=0;    					       	 // il percorso si riferisce ad una cartella dove posso scrivere? (no=0, sì=1)
	if ( ! SendData(sock_client, &risp, sizeof(int)) )   //  3) comunico al server se posso accedere al path specificato
		return;		
	if (risp==0) {						      	// comunico all'utente che il path indicato per salvare il file non è utilizzabile
		fprintf (stderr, REDf"- "MAGb WHIf"%s"RST REDf": questo percorso non esiste o non si hanno permessi per accedervi.\n"RST, path);
		return;	
	}	
	if ( ! ReceiveData (sock_client, &y, NULL) )   // 4) il file compresso è creato e accessibile al server (quindi inviabile)? Sì[y=1] oppure No[y=0]. 
		return;		
	if (y==0) {
		fprintf (stderr, REDf"- Il server non e' stato in grado di creare o accedere al file compresso.\n"RST);
		return;
	}	
	if ( ! ReceiveData (sock_client, &size, NULL) )  // 5) ricezione dimensione archivioo (ridondante: la potrei ricevere con la ReceiveData > 
		return;					 // > dopo, ma quando la invoco devo già sapere quanto è grande il buffer dove metterne il contenuto
	tmp_archive = malloc(size);  	    // alloco in memoria dinamica uno spazio sufficiente (buffer) a contenere l'archivio che mi invierà il server 
	if ( ! ReceiveData (sock_client, tmp_archive, NULL) )// 6) ricezione contenuto dell'archivio compresso
		return;		 
	strcat(path, temp); 	       	 // creo il path completo dell'archivio (locale) aggiugendovi alla fine (append) il nome dell'archivio (in temp)
	fp = fopen(path, "wb");    					 // creo il file locale (archivio), per ora vuoto
	if(fp == NULL){
		free(tmp_archive);
		fprintf (stderr, REDf"- Impossibile creare il file-archivio nel percorso indicato.\n"RST); //errore di creazione
		risp=0;
		if ( !SendData(sock_client, &risp, sizeof(int)))      // 7e) comunico al server che la creazione dell'archivio lato client è fallita (0) 
			return; 
		return;                                          
	}
	fwrite(tmp_archive, 1, size, fp);       			  // scrivo sul file l'archivio ricevuto
	risp=1;	
	if ( ! SendData(sock_client, &risp, sizeof(int)) )    // 7) comunico al server la creazione dell'archivio lato client è riuscita (1)
		return;		
	fflush(fp);			 // chiudo lo stream, elimino il buffer e stampo a video l'esito positivo della sCompress
	fclose(fp);
	free(tmp_archive);
	printf(CYAf"- Archivio "GREf"%s"CYAf" ricevuto con successo.\n"RST, temp); 
}

 
 // MAIN
int main ( int argc, char* argv[] )   /* corpo del processo client: per lanciarlo si usa "compressor-client <host remoto> <porta>" */
{	
	char *IPv4address_string; 							// stringa corrispondente all'indirizzo (IPv4) del server 
	int port, sock_client, c;                		// porta su cui il server è in ascolto, socket descriptor del client, un intero
	struct sockaddr_in server_address; 				// indirizzo del server (IPv4)
	int quitexit=0;
	if (argc!=3) { 										// controllo numero argomenti
	        fprintf (stderr, REDf"\nIl programma compressor-client deve essere lanciato specificando, nell'ordine,"); 
                fprintf(stderr,"l'indirizzo IPv4 della macchina dove gira il server e la porta su cui esso e' in ascolto."RST"\n\n");
		return 0;
	}                     			        	// memorizzo i parametri del programma inseriti da riga di comando invocandolo: 
	IPv4address_string = argv[1];    			        // stringa relativa ai 4 ottetti dell'indirizzo IPv4
	port = atoi(argv[2]);           					// porta (il client dovrà conoscere su quale il server sta ascoltando)
	if (inet_addr(IPv4address_string)==INADDR_NONE) {   // controllo formato indirizzo IP
		if (strcmp(IPv4address_string,"localhost")==0)
			strcpy(IPv4address_string,"127.0.0.1");    // gestione "speciale" per l'indirizzo di loopback (utile in fase di test)
		else {
			fprintf (stderr, REDf"\nIndirizzo IPv4 non valido (quattro numeri tra 0 e 255 separati da punto)."RST"\n\n");
			return 0;
		}
	}
	if ( (port<1024)||(port>65535) ) {   				// controllo porta
		fprintf (stderr, REDf"\nNumero porta non valido (intero compreso tra 1024 e 65535)."RST"\n\n");
		return 0;
	}
	memset( &server_address, 0, sizeof(server_address) ); // preparazione indirizzo e porta del server (3 passi)
	server_address.sin_family = AF_INET;   								  // I) sin family (indirizzo IPv4) 
	c = inet_pton( AF_INET, IPv4address_string, &server_address.sin_addr ); // II) sin addr da notazione puntata a numerica
	if (c==0) {
		fprintf (stderr, REDf"Indirizzo non convertibile in formato numerico."RST"\n");
		return 0; 
	}
	server_address.sin_port = htons(port); 		       	  // III) network ordering del port number (Short): da formato di host (H) a rete (N) 
	sock_client = socket( PF_INET, SOCK_STREAM, 0 ); 		 // creazione del socket del client (ha solo questo)
	if (sock_client==-1) {
		fprintf (stderr, REDf"Impossibile creare il socket."RST"\n");   
		return 0; 										 // errore creazione socket: il client termina 
	}
	printf(CYAf"\nConnessione al server in corso..."RST"\n");
	c = connect( sock_client, (const struct sockaddr*)&server_address, sizeof(struct sockaddr_in) ); 
	if (c!=0) {										 	 // richiesta connessione al server
		fprintf (stderr, REDf"-Connessione al server fallita (controllare indirizzo e porta)."RST"\n\n");
		return 0;
	} 						        	 // qui c=0, ma subito dopo lo sovrascrivo (ma non mi interessa cosa c'è in c)
	if ( ! ReceiveData(sock_client, &c, NULL) ) 		 // 1) il server mi informa che mi è stato assegnato un thread del pool
		return 0;	
	printf ("\n"REDb WHIf"REMOTE COMPRESSOR client, v %s"RST"\n", VERSION);
	printf (CYAf"- Connesso al server "GREf"%s"CYAf" sulla porta "GREf"%d"CYAf".\n"RST, IPv4address_string, port);
	printf ("Digitare "GREf"help"RST" per visualizzare i comandi disponibili.\n");
	printf (CYAf"- ATTENZIONE:"RST"\n        *inserire comandi di lunghezza massima "GREf"%d"RST" caratteri.\n", MAX_MSG_LEN);
	printf ("        *racchiudere i nomi contententi spazi tra virgolette ("GREf"\""RST".."GREf"\""RST")\n"); 
	while(1){                      	           // ciclo di invio comandi al server (pool thread) fino a che non c'è la quit (programma interattivo)
		char clientCommand[MAX_MSG_LEN+1];
		int choice, len; 
		quitexit=0;
		printf( YELf"%s"RST, PROMPT );  					 // prompt a schermo
		fgets( clientCommand, MAX_MSG_LEN, stdin ); 	 // ricezione comando scritto dal client da tastiera 
		strcpy( clientCommand, trim_side_spaces(clientCommand) ); // tolgo gli spazi a sx e dx del comando
		len = strlen( clientCommand );		
		if (len==0)  			       	 // se il comando è vuoto ricomincio col prompt saltando alla prossima iterazione del ciclo while
			continue;		 						
		if ( ! SendData( sock_client, &clientCommand, len) )  // 2) informo il server del comando eseguito dall'utente-client (privo del NUL finale)
			break;			
		if ( ! ReceiveData (sock_client, &choice, NULL) )// 3) ricevo dal server il numero d'ordine del comando ricevuto (0-7) 
			break;			
		switch (choice){// A seconda del comando eseguo azioni diverse (invoco una funzione specifica, tranne per la quit)
			case 0: // comando non valido
			case 1: // help
			case 2: // configure-compressor
			case 3: // configure-name
			case 4: // show-configuration
			case 7: //show-list
			case 8:{//empty-list 
				cCMDS0_478(sock_client); // help,show-c,config-n,config-c e il caso di comando non valido prevedono solo >
				continue;                // > che il client riceva il messaggio da stampare dal server e lo mandi a video
			}
			case 5:{ //send
				int counter, i;
				if ( ! ReceiveData (sock_client, &counter, NULL) )  //  0)  memorizzo quanti file devo inviare al server (n° di cSend)
					break;
				for (i=0;i<counter;i++)	       	// alcuni di questi path potrebbero riferirsi a file non esistenti o non accessibili, 
					cSEND (sock_client);  				// ma devo comunque tentare gli invii [chiamare le cSEND])
				continue;
			}
			case 6: { //compress
				cCOMPRESS (sock_client);
				continue;
			}
			case 9:{ //quit
				quitexit=1;    	// così posso distinguere i casi in cui il ciclo while termina per quit o per disconnessione dal server
				break;        // esco dallo switch (farò subito la chiusura del socket con il server)
			}
		} //fine switch
		break;  			      	// se esco dallo switch (per quit o per caduta del server) esco anche dal while 		
	} //fine while
    if (!quitexit) 
	   printf(REDf"Errore con la connessione: il server non risponde\n");
    c = shutdown(sock_client, SHUT_RDWR);		// chiudo il socket del client
    if (c<0)
	   perror(REDf"shutdown"RST);
    c = close(sock_client);  			  				 
    if (c<0)
	   perror(REDf"close"RST);
    printf(REDb WHIf"Terminazione REMOTE COMPRESSOR client."RST"\n\n");
	 return 0; 
}  //fine main del client (terminazione applicativo lato client)
