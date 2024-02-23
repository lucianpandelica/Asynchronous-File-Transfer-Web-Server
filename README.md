# Asynchronous File Transfer Web Server

## Implementare:

Am pornit implementarea de la fisierele suport puse la dispozitie in arhiva temei
si am folosit pe parcurs unele dintre functiile wrapper puse la dispozitie.

Tema foloseste la baza o structura handler pentru conexiuni, in cadrul careia se
afla campuri ce retin date proprii acesteia, printre care socket file descriptor-ul
aferent conexiunii, state-ul conexiunii, bufferul de intrare, header-ul de fisier
aferent request-ului primit, file descriptor-ul fisierului aferent, contoare pentru
numarul de octeti ce trebuie trimisi, impreuna cu date specifice lucrului cu fisiere
dinamice: variabila io_context_t, I/O event file descriptor si state aferent
operatiei asincrone.

De asemenea, am adaugat la epoll-ul ce sta la baza serverului, pe parcursul
implementarii, doar evenimente ce retin file descriptori. Astfel, putem primi la
un moment dat un file descriptor aferent unui socket al conexiunii corespunzatoare
sau un file descriptor aferent operatiei I/O asincrone curente a unei conexiuni
(eveniment ce va avea rol de notificare).

Am folosit un vector cu numar dinamic de elemente de tip structura 'conn_fd_map',
ce retine perechi de tip (file descriptor, conexiune), de doua tipuri:

- (file descriptor eveniment I/O, conexiune);
- (file descriptor socket, conexiune).

Cum fiecare conexiune are un socket file descriptor si un I/O events file descriptor,
putem identifica la primirea unui eveniment, prin parcurgerea vectorului, daca acesta
reprezinta o notificare ca o operatie I/O asincrona s-a finalizat sau faptul ca se
doreste scrierea / citirea pe socketul unei conexiuni.

De asemenea, manipulam pe parcursul rularii programului, in functie de stadiul in
care se afla prelucrarea request-ului si tipul de fisier cerut, tipul evenimentului
asociat socket-ului conexiunii: IN sau OUT.

## Desfasurare program:

La pornirea serverului, cream o instanta epoll, un socket cu rol pasiv asociat
serverului si adaugam fd-ul celui din urma in lista de interese a epoll.

Intram apoi in bucla while infinita, unde asteptam aparitia unui eveniment. Daca
file descriptor-ul asociat evenimentului este cel al serverului, acceptam o noua
conexiune, ii instantiem structura si adaugam socket fd-ul la lista de interese
a epoll pentru evenimente IN. Altfel, daca evenimentul precizeaza un file
descriptor diferit, identificam tipul sau (socket fd sau I/O event fd si procedam
corespunzator). De asemenea, la crearea unei conexiuni, marcam socket-ul asociat
drept non-blocant.

Pentru un socket fd, mergem catre functiile de citire sau de scriere in functie
de stadiul prelucrarii request-ului, avand aceeasi functie de citire pentru orice
fisier si functii de scriere diferite intre cele doua tipuri de fisiere.

In cazul citirii, preluam de la socket parti din request-ul trimis la tastatura
si compunem comanda intr-un buffer propriu conexiunii, aferent comenzilor. Stim
ca am citit toata comanda atunci cand identificam ca ultimele 4 caractere sunt
"\r\n\r\n", caz in care o parsam si identificam calea catre fisier, folosind
HTTP Parser-ul din arhiva de resurse a temei. Tot la aceasta etapa identificam
si tipul de fisier cerut si retinem rezultatul in structura conexiunii. Daca
avem de a face cu un fisier dinamic, initializam variabila io_context_t
corespunzatoare.

Pentru trimitere, in cazul fisierelor dinamice, apelam mai intai functia de
trimitere a header-ului, de fiecare data cand evenimentul asociat conexiunii
revine in epoll si nu a terminat inca de trimis header-ul. La aceasta etapa
deschidem fisierul, verificam daca s-a gasit si compunem header-ul, in
functie de cazul necesar: HTTP 200 sau HTTP 404. Daca s-a gasit fisierul,
si am terminat de scris header-ul, dam submit unei operatii de citire pe fisier
si adaugam fd-ul evenimentului I/O asteptat la epoll.

La primirea notificarii, la un pas ulterior, preluam evenimentul cu 'io_getevents',
marcam faptul ca scrierea este pregatita si eliminam fd-ul asociat evenimentului
din epoll, apoi scriem parti din fisier pana cand am scris tot bufferul citit
asincron si facem un nou submit pentru citire, reluand pasii de mai sus. Scriem
fisierul folosind functia 'send'.

Pentru trimiterea fisierului in cazul fisierelor statice, folosim o functie unica
ce se ocupa atat de trimiterea header-ului, cat si de trimiterea fisierului.
Marcam stadiile in care se afla trimiterea folosind campul 'state' al conexiunii
si identificam la fiecare intrare in functia de send ce pas avem de executat,
continuand de unde am ramas la ultimul apel. Scriem fisierul folosind functia
'sendfile', cu parametrul asociat offset-ului setat pe NULL, pentru a folosi
offset-ul actualizat pe parcursul scrierii.

## Git:

https://github.com/systems-cs-pub-ro/so/tree/2022-2023/teme/assignments/3-aws

## Bibliografie:

https://man7.org/linux/man-pages/man2/recv.2.html
https://man7.org/linux/man-pages/man2/send.2.html
https://man7.org/linux/man-pages/man2/sendfile.2.html
https://man7.org/linux/man-pages/man2/io_setup.2.html
https://man7.org/linux/man-pages/man2/io_submit.2.html
https://man7.org/linux/man-pages/man2/io_getevents.2.html
https://man7.org/linux/man-pages/man2/io_destroy.2.html
https://ocw.cs.pub.ro/courses/so/laboratoare/laborator-11
https://ocw.cs.pub.ro/courses/so/cursuri/curs-10
https://ocw.cs.pub.ro/courses/so/cursuri/curs-11
http://elf.cs.pub.ro/so/res/teme/tema5-util/lin/samples/
https://github.com/nodejs/http-parser
