# CCScompressor
C client server file compressor: 

The project consists in a client / server C application. Clients and servers communicate through TCP sockets with a competing server implemented with POSIX Thread. The main thread remains permanently awaiting new connections and refers to a pool of threads handling requests. The remote-compressor client allows to send files to one Server, then returning an archive (tar), compressed with a compression algorithm chosen by Client, containing those files. 

The compressor-server program is an interactive program that allows the client:
· The choice of the compression algorithm
· The name of the archive
· The ability to view selected configurations
· Sending one or more files to the server
· Receiving a compressed archive (tar) with the files sent by the client itself
The command to open a work session has the following syntax:
" compressor-client <remote-host> <port>"
Then the user can type commands to interact with the server:
· Help: This command must show video a short command of the available commands.
· Configure-compressor [compressor]: this command must configure the server in so
· Configure-name [name]: set the name of the archive 
· Show-configuration: returns the name chosen for the archive
· Send [file]: this command takes as a parameter the path of one or more local files that must be sent to the server
· Compress [path]: creates the archives and send them to the client
· Quit: This command causes the session to terminate with the command

The compressor-server process represents the remote-compressor service server. this The process persists in listening to client requests from connectivity. When a Client connects, compressor-server must activate a thread from the pool to delegate the management of the service and must wait for other connection requests. 
The syntax of the compressor-server command is as follows:
" compressor-server <port>"
Where port is the port on which the server is listening. 

Current state:
Compile command
* gcc -Wall -pthread -o s compressor-server.c
* gcc -Wall -o c compressor-client.c
Unix OS only
CLI UI
gnuzip, bzip2, xz, compress
