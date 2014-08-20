/*       teleinfo2014_v1.c                              												*/
/* Version pour PC et wrt54gl                              												*/
/* Lecture données Téléinfo et enregistre données sur base mysql vesta si ok sinon dans fichier csv.   	*/
/* Connexion par le port série du Wrt54gl (Console désactivée dans inittab.)            				*/
/* Vérification checksum données téléinfo et boucle de 3 essais si erreurs.            					*/
/* Par domos78 at free point fr                              											*/

/*
Paramètres à adapter:
- Port série à modifier en conséquence avec SERIALPORT.
- Nombre de valeurs à relever: NB_VALEURS + tableaux "etiquettes" et "poschecksum" à modifier selon abonnement (ici triphasé heures creuses).
- Paramètres Mysql (Serveur, Base, table et login/password)
- Autorisé le serveur MySql à accepter les connexions distantes pour le Wrt54gl.

Compilation : se connecter en ssh, se mettre dans le repertoire où est placé le fichier teleinfoserial_mysql.c et lancer la commande :
- gcc -Wall teleinfo2014_v1.c -o teleinfo2014_v1 -lmysqlclient

Compilation wrt54gl:
- avec le SDK (OpenWrt-SDK-Linux).

Résultat pour les données importantes dans la base MySql du serveur distant:
dan@vesta:~$ bin/listdatateleinfo.sh
timestamp       rec_date        rec_time           hchc    hchp         ptec    inst     papp
1222265525      24/09/2008      16:12:05        8209506 8026019 HP      1      460
1222265464      24/09/2008      16:11:04        8209499 8026019 HP      1      460
1222265405      24/09/2008      16:10:05        8209493 8026019 HP      1      390
1222265344      24/09/2008      16:09:04        8209487 8026019 HP      1      390
1222265284      24/09/2008      16:08:04        8209481 8026019 HP      1      390
1222265225      24/09/2008      16:07:05        8209476 8026019 HP      1      390
1222265164      24/09/2008      16:06:04        8209470 8026019 HP      1      390
1222265105      24/09/2008      16:05:05        8209464 8026019 HP      1      390

Résultat en mode DEBUG:
root@wrt54gl:~# ./teleinfoserial_mysql
----- 2008-10-12 15:59:52 -----
ADCO='70060936xxxx'
OPTARIF='HC..'
ISOUSC='20'
HCHC='008444126'
HCHP='008228815'
PTEC='HP'
IINST='002'
IMAX='019'
PAPP='00610'
HHPHC='E'
MOTDETAT='000000'
*/

//-----------------------------------------------------------------------------
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include <termios.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <mysql/mysql.h>

// Define port serie
#define BAUDRATE B1200
#define SERIALPORT "/dev/ttyAMA0"

// Define mysql
#define MYSQL_HOST "srv01.jd-multimedia.fr"
#define MYSQL_DB "teleinfo_pi"
#define MYSQL_TABLE "teleinfo"
#define MYSQL_LOGIN "teleinfo_pi"
#define MYSQL_PWD "teleinfo_pi"

// Fichier local au Wrt4gl/PC + fichier trame pour debug.
#define DATACSV "/tmp/teleinfosql.csv"
#define TRAMELOG "/tmp/teleinfotrame."

/* Active mode debug. Décommenter pour activer le mode Debug. Il écrit :
- chaque cheksum lu et calculé dans "/var/log/messages"
- "Requete MySql ok." dans "/var/log/messages" pour chaque écriture conforme dans la BDD
- un fichier avec la trame de données pour chaque lecture du compteur dans "/var/www/teleinfo/trames/"
*/
//#define DEBUG

//-----------------------------------------------------------------------------

// Déclaration pour le port série.
int             fdserial ;
struct termios  termiosteleinfo ;

// Déclaration pour les données.
char ch[2] ;
char car_prec ;
char message[512] ;
char* match;
int id ;
char datateleinfo[512] ;

/// Constantes/Variables à changées suivant abonnement, Nombre de valeurs, voir tableau "etiquettes", 20 pour abonnement tri heures creuse.
#define NB_VALEURS 11
char etiquettes[NB_VALEURS][11] = {"ADCO", "OPTARIF", "ISOUSC", "HCHC", "HCHP", "PTEC", "IINST", "IMAX", "PAPP", "HHPHC", "MOTDETAT"} ;
// Fin Constantes/variables à changées suivant abonnement.

char    valeurs[NB_VALEURS][11] ;
char    checksum[255] ;
int    res ;
int   no_essais = 1 ;
int   nb_essais = 3 ;
int   erreur_checksum = 0 ;

// Déclaration pour la date.
time_t       td;
struct    tm    *dc;
char      sdate[12];
char      sheure[10];
char      timestamp[11];
char      ftimestamp[21];

/*------------------------------------------------------------------------------*/
/* Init port rs232                        */
/*------------------------------------------------------------------------------*/
int initserie(void)
// Mode Non-Canonical Input Processing, Attend 1 caractère ou time-out(avec VMIN et VTIME).
{
   int device ;

        // Ouverture de la liaison serie (Nouvelle version de config.)
        if ( (device=open(SERIALPORT, O_RDWR | O_NOCTTY)) == -1 )
   {
                syslog(LOG_ERR, "Erreur ouverture du port serie %s !", SERIALPORT);
                exit(1) ;
        }

        tcgetattr(device,&termiosteleinfo) ;            // Lecture des parametres courants.

   cfsetispeed(&termiosteleinfo, BAUDRATE) ;         // Configure le débit en entrée/sortie.
   cfsetospeed(&termiosteleinfo, BAUDRATE) ;

   termiosteleinfo.c_cflag |= (CLOCAL | CREAD) ;         // Active réception et mode local.

  // Format série "7E1"
  termiosteleinfo.c_cflag |= PARENB  ;            // Active 7 bits de donnees avec parite pair.
  termiosteleinfo.c_cflag &= ~PARODD ;
  termiosteleinfo.c_cflag &= ~CSTOPB ;
  termiosteleinfo.c_cflag &= ~CSIZE ;
  termiosteleinfo.c_cflag |= CS7 ;
  termiosteleinfo.c_iflag |= (INPCK | ISTRIP) ;         // Mode de control de parité.
  termiosteleinfo.c_cflag &= ~CRTSCTS ;            // Désactive control de flux matériel.
  termiosteleinfo.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG) ;   // Mode non-canonique (mode raw) sans echo.
  termiosteleinfo.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL) ;   // Désactive control de flux logiciel, conversion 0xOD en 0x0A.
  termiosteleinfo.c_oflag &= ~OPOST ;            // Pas de mode de sortie particulier (mode raw).
  termiosteleinfo.c_cc[VTIME] = 80 ;              // time-out à ~8s.
  termiosteleinfo.c_cc[VMIN]  = 0 ;               // 1 car. attendu.
  tcflush(device, TCIFLUSH) ;               // Efface les données reçues mais non lues.
  tcsetattr(device,TCSANOW,&termiosteleinfo) ;         // Sauvegarde des nouveaux parametres
  return device ;
}

/*------------------------------------------------------------------------------*/
/* Lecture données téléinfo sur port série               */
/*------------------------------------------------------------------------------*/
void LiTrameSerie(int device)
{
// (0d 03 02 0a => Code fin et début trame)
   tcflush(device, TCIFLUSH) ;         // Efface les données non lus en entrée.
   message[0]='\0' ;
   memset(valeurs, 0x00, sizeof(valeurs)) ;

   do
   {
      car_prec = ch[0] ;
      res = read(device, ch, 1) ;
      if (! res)
      {
         syslog(LOG_ERR, "Erreur pas de réception début données Téléinfo !\n") ;
         close(device);
         exit(1) ;
      }
    }
   while ( ! (ch[0] == 0x02 && car_prec == 0x03) ) ;   // Attend code fin suivi de début trame téléinfo .

   do
   {
      res = read(device, ch, 1) ;
      if (! res)
      {
         syslog(LOG_ERR, "Erreur pas de réception fin données Téléinfo !\n") ;
         close(device);
         exit(1) ;
      }
      ch[1] ='\0' ;
      strcat(message, ch) ;
   }
   while (ch[0] != 0x03) ;            // Attend code fin trame téléinfo.
}

/*------------------------------------------------------------------------------*/
/* Test checksum d'un message (Return 1 si checkum ok)            */
/*------------------------------------------------------------------------------*/
int checksum_ok(char *etiquette, char *valeur, char checksum)
{
   unsigned char sum = 32 ;      // Somme des codes ASCII du message + un espace
   int i ;

   for (i=0; i < strlen(etiquette); i++) sum = sum + etiquette[i] ;
   for (i=0; i < strlen(valeur); i++) sum = sum + valeur[i] ;
   sum = (sum & 63) + 32 ;

   //placé ici, permet d'enregistrer le cheksum lu et calculé à chaque fois dans le fichier "/var/log/messages"
   #ifdef DEBUG
      syslog(LOG_INFO, "Checksum lu:%02x   calculé:%02x", checksum, sum) ;
   #endif

   if ( sum == checksum) return 1 ;   // Return 1 si checkum ok.
   return 0 ;
}

/*------------------------------------------------------------------------------*/
/* Recherche valeurs des étiquettes de la liste.            */
/*------------------------------------------------------------------------------*/
int LitValEtiquettes()
{
   int id ;
   erreur_checksum = 0 ;

   for (id=0; id<NB_VALEURS; id++)
   {
      if ( (match = strstr(message, etiquettes[id])) != NULL)
      {
         sscanf(match, "%s %s %s", etiquettes[id], valeurs[id], checksum) ;
         if ( strlen(checksum) > 1 ) checksum[0]=' ' ;   // sscanf ne peux lire le checksum à 0x20 (espace), si longueur checksum > 1 donc c'est un espace.
         if ( ! checksum_ok(etiquettes[id], valeurs[id], checksum[0]) )
         {
            //syslog(LOG_ERR, "Donnees teleinfo [%s] corrompues (essai %d) !\n", etiquettes[id], no_essais) ;
            syslog(LOG_INFO, "Donnees teleinfo [%s] corrompues (essai %d) !\n", etiquettes[id], no_essais) ;
            erreur_checksum = 1 ;
            return 0 ;
         }
      }
      else // code ajouté L253 à 258 inclus
      {
      return 0;
      syslog(LOG_INFO, "pb de lecture de la valeur de ADCO (étiquette 0)") ;
      for (id=0; id<NB_VALEURS; id++) syslog(LOG_INFO,"%s='%s'\n", etiquettes[id], valeurs[id]) ; // affiche les etiquettes + caleurs si erreur sur etiquette 0
      }
   }
   // Remplace chaine "HP.." ou "HC.." par "HP ou "HC".
   valeurs[5][2] = '\0' ;
   #ifdef DEBUG
   printf("----------------------\n") ; for (id=0; id<NB_VALEURS; id++) printf("%s='%s'\n", etiquettes[id], valeurs[id]) ;
   #endif
   return 1 ;
}

/*------------------------------------------------------------------------------*/
/* Test si dépassement intensité                  */
/*------------------------------------------------------------------------------*/
/* int DepasseCapacite()
{
   //  Test sur les 3 phases (étiquette ADIR1, ADIR2, ADIR3) à remplacer par ADPS pour monophasé.
   if ( (strlen(valeurs[17])) || (strlen(valeurs[18])) || (strlen(valeurs[19])) )
   {
      syslog(LOG_INFO, "Dépassement d'intensité: ADRI1='%s', ADRI2='%s', ADRI3='%s' !", valeurs[17], valeurs[18], valeurs[19]) ;
      return 1 ;
   }
   return 0 ;
}
*/

/*------------------------------------------------------------------------------*/
/* Test si une des valeurs est nulle (mauvaise acquisition des datas)                  */
/*------------------------------------------------------------------------------*/
/*int Erreur_acqusition()
{
   //  Test sur les étiquettes pour monophasé.
   if ( valeurs[0]==NULL || valeurs[1]==NULL || valeurs[2]==0 || valeurs[3]==0 || valeurs[4]==0 || valeurs[5]==NULL || valeurs[6]==0 || valeurs[7]==0 || valeurs[8]==0 || valeurs[9]==NULL || valeurs[10]==NULL )
   {
      syslog(LOG_INFO, "erreur d'acquisition: ADCO='%s', OPTARIF='%s', ISOUSC='%s', HCHC='%s', HCHP='%s', PTEC='%s', IINST='%s', IMAX='%s', PAPP='%s', HHPHC='%s', MOTDETAT='%s' !", valeurs[0], valeurs[1], valeurs[2], valeurs[3], valeurs[4], valeurs[5], valeurs[6], valeurs[7], valeurs[8], valeurs[9], valeurs[10]) ;
      return 1 ;
   }
   return 0 ;
}
*/
/*------------------------------------------------------------------------------*/
/* Ecrit les données teleinfo dans base mysql               */
/*------------------------------------------------------------------------------*/
int writemysqlteleinfo(char data[])
{
   MYSQL mysql ;
   char query[255] ;

   /* INIT MYSQL AND CONNECT ----------------------------------------------------*/
   if(!mysql_init(&mysql))
   {
      syslog(LOG_ERR, "Erreur: Initialisation MySQL impossible !") ;
      return 0 ;
   }
   if(!mysql_real_connect(&mysql, MYSQL_HOST, MYSQL_LOGIN,   MYSQL_PWD, MYSQL_DB, 0, NULL, 0))
   {
      syslog(LOG_ERR, "Erreur connection %d: %s \n", mysql_errno(&mysql), mysql_error(&mysql));
      return 0 ;
   }

   sprintf(query, "INSERT INTO %s VALUES (%s)", MYSQL_TABLE, data);

   if(mysql_query(&mysql, query))
   {
      syslog(LOG_ERR, "Erreur INSERT %d: \%s \n", mysql_errno(&mysql), mysql_error(&mysql));
      mysql_close(&mysql);
      return 0 ;
   }
   #ifdef DEBUG
   else syslog(LOG_INFO, "Requete MySql ok.") ;
   #endif
   mysql_close(&mysql);
   return 1 ;
}

/*------------------------------------------------------------------------------*/
/* Ecrit les données teleinfo dans fichier DATACSV            */
/*------------------------------------------------------------------------------*/
void writecsvteleinfo(char data[])
{
        /* Ouverture fichier csv */
        FILE *datateleinfo ;
        if ((datateleinfo = fopen(DATACSV, "a")) == NULL)
        {
      syslog(LOG_ERR, "Erreur ouverture fichier teleinfo %s !", DATACSV) ;
                exit(1);
        }
        fprintf(datateleinfo, "%s\n", data) ;
        fclose(datateleinfo) ;
}

/*------------------------------------------------------------------------------*/
/* Ecrit la trame teleinfo dans fichier si erreur (pour debugger)      */
/*------------------------------------------------------------------------------*/
void writetrameteleinfo(char trame[], char ts[])
{
   char nomfichier[255] = TRAMELOG ;
   strcat(nomfichier, ts) ;
        FILE *teleinfotrame ;
        if ((teleinfotrame = fopen(nomfichier, "w")) == NULL)
        {
      syslog(LOG_ERR, "Erreur ouverture fichier teleinfotrame %s !", nomfichier) ;
                exit(1);
        }
        fprintf(teleinfotrame, "%s", trame) ;
        fclose(teleinfotrame) ;
}


/*------------------------------------------------------------------------------*/
/* Main                              */
/*------------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
openlog("teleinfo2014_v1", LOG_PID, LOG_USER) ;
fdserial = initserie() ;

do
{
   // Lit trame téléinfo.
   LiTrameSerie(fdserial) ;

   time(&td) ;                                     //Lit date/heure système.
   dc = localtime(&td) ;
   strftime(sdate,sizeof sdate,"%Y-%m-%d",dc);
   strftime(sheure,sizeof sdate,"%H:%M:%S",dc);
   strftime(timestamp,sizeof timestamp,"%s",dc);
   strftime(ftimestamp,sizeof ftimestamp,"%Y-%m-%d %H:%M:%S",dc);

   #ifdef DEBUG
   writetrameteleinfo(message, timestamp) ;   // Enregistre trame en mode debug.
   #endif

   //Erreur_acqusition();           // Test si erreur d'acquisition.

   if ( LitValEtiquettes() )          // Lit valeurs des étiquettes de la liste.
   {
      //if(strcmp(valeurs[0],"")!=0)  // code ajouté = lignes 385 à 389.
      //{
      sprintf(datateleinfo,"'%s','%s','%s','%s','%s','%s','%s','%s','%s','%s','%s','%s','%s','%s','%s'", timestamp, ftimestamp, sdate, sheure, valeurs[0], valeurs[1], valeurs[2], valeurs[3], valeurs[4], valeurs[5], valeurs[6], valeurs[7], valeurs[8], valeurs[9], valeurs[10]) ;
      //}
      //else
      //{
      //syslog(LOG_INFO, "pb de lecture de la valeur de ADCO (étiquette 0)") ;
      //for (id=0; id<NB_VALEURS; id++) syslog(LOG_INFO,"%s='%s'\n", etiquettes[id], valeurs[id]) ; // affiche les etiquettes + caleurs si erreur sur etiquette 0
      //writetrameteleinfo(message, timestamp) ;   // Enregistre trame
      //}
      if (! writemysqlteleinfo(datateleinfo) ) writecsvteleinfo(datateleinfo) ;      // Si écriture dans base MySql KO, écriture dans fichier csv.
      //DepasseCapacite() ;          // Test si etiquette dépassement intensité (log l'information seulement).
      //Erreur_acqusition();           // Test si erreur d'acquisition.
   }
   /*#ifdef DEBUG
   else writetrameteleinfo(message, timestamp) ;   // Si erreur checksum enregistre trame.
   #endif */
   no_essais++ ;
}
while ( (erreur_checksum) && (no_essais <= nb_essais) ) ;

close(fdserial) ;
closelog() ;
exit(0) ;
}
