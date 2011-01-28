
// protocol.cpp

// includes

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "board.h"
#include "book.h"
#include "eval.h"
#include "fen.h"
#include "material.h"
#include "move.h"
#include "move_do.h"
#include "move_legal.h"
#include "option.h"
#include "pawn.h"
#include "posix.h"
#include "probe.h"
#include "protocol.h"
#include "pst.h"
#include "search.h"
#include "trans.h"
#include "util.h"

// constants

static const double NormalRatio = 1.0;
static const double PonderRatio = 1.0;

// variables

#ifdef _WIN32
CRITICAL_SECTION CriticalSection;
#else
pthread_mutex_t CriticalSection=PTHREAD_MUTEX_INITIALIZER;
#endif

int NumberThreads = 1;

static bool Init;

static bool Searching; // search in progress?
static bool Infinite; // infinite or ponder mode?
static bool Delay; // postpone "bestmove" in infinite/ponder mode?

static char * dirptr = "C:/egbb";

// prototypes

static void init              ();
static void loop_step         ();

static void parse_go          (char string[]);
static void parse_position    (char string[]);
static void parse_setoption   (char string[]);

static void send_best_move    ();

static bool string_equal      (const char s1[], const char s2[]);
static bool string_start_with (const char s1[], const char s2[]);

// functions

void book_parameter() {

    // UCI options

	book_close();
	if (option_get_bool("OwnBook")) {
         book_open(option_get_string("BookFile"));
	}


}

// loop()

void loop() {

   // init (to help debugging)

   Init = false;

   Searching = false;
   Infinite = false;
   Delay = false;

   search_clear();

   board_from_fen(SearchInput->board,StartFen);

   // loop

   while (true) loop_step();
}

// profile()

void profile() {
   init();
   board_from_fen(SearchInput->board,"r2q1rk1/pp1bppbp/2np1np1/8/2BNP3/2N1BP2/PPPQ2PP/2KR3R b - - 0 10");
   SearchInput->depth_is_limited = false;
   SearchInput->infinite = true;
   SearchInput->time_is_limited = false;
   search_clear();
   search();
}

// init_threads()

void init_threads(bool call_option) {
#ifdef _WIN32
   SYSTEM_INFO sys_info;
#endif
   int thread_count, option_max;
#ifdef _WIN32
   GetSystemInfo(&sys_info);
   thread_count = sys_info.dwNumberOfProcessors;
#else
#ifdef _SC_NPROCESSORS_ONLN
   thread_count=sysconf(_SC_NPROCESSORS_ONLN);
#else
//   thread_count=1;
#endif
#endif
   if (thread_count > MaxThreads) thread_count = MaxThreads;
   if (!MP) thread_count = 1;
   if (call_option) {
      option_max = option_get_int("Threads");
	  if (thread_count > option_max) thread_count = option_max;
   }
   NumberThreads = thread_count;
}

// init()

static void init() {
	
   if (!Init) {

      // late initialisation

      Init = true;
		
	  book_parameter();

      trans_alloc(Trans);

      pawn_init();
      pawn_alloc();

      material_init();
      material_alloc();

      pst_init();
      eval_init();

#ifdef _WIN32
	  InitializeCriticalSection(&CriticalSection);
#endif
	  SearchInput->exit_engine = false;
	  start_suspend_threads();
   }

}

// event()

void event() {

   while (!SearchInfo[0]->stop && input_available()) loop_step();
}

// loop_step()

static void loop_step() {

   char string[65536];
   int ThreadId;

   // read a line

   get(string,65536);

/* FILE *fp;
   fp=fopen("toga.log","a+");
   fprintf(fp,"%s\n",string);
   fclose(fp); */

   // parse

   if (false) {

   } else if (string_start_with(string,"debug ")) {

      // dummy

   } else if (string_start_with(string,"go ")) {

      if (!Searching && !Delay) {
         init();
         parse_go(string);
      } else {
         ASSERT(false);
      }

   } else if (string_equal(string,"isready")) {

      if (!Searching && !Delay) {
         init();
      }

      send("readyok"); // no need to wait when searching (dixit SMK)

   } else if (string_equal(string,"ponderhit")) {

      if (Searching) {

         ASSERT(Infinite);

         SearchInput->infinite = false;
         Infinite = false;

      } else if (Delay) {

         send_best_move();
         Delay = false;

      } else {

         ASSERT(false);
      }

   } else if (string_start_with(string,"position ")) {

      if (!Searching && !Delay) {
         init();
         parse_position(string);
      } else {
         ASSERT(false);
      }

   } else if (string_equal(string,"quit")) {

      ASSERT(!Searching);
      ASSERT(!Delay);

	  SearchInput->exit_engine = true;
	  resume_threads();
	    eval_quit();
      exit(EXIT_SUCCESS);

   } else if (string_start_with(string,"setoption ")) {

      if (!Searching && !Delay) {
         parse_setoption(string);
		 pawn_parameter();
		 material_parameter();
		 book_parameter();
		 pst_init();
		 eval_parameter();
      } else {
         ASSERT(false);
      }

   } else if (string_equal(string,"stop")) {

      if (Searching) {

			for (ThreadId = 0; ThreadId < NumberThreads; ThreadId++){
				SearchInfo[ThreadId]->stop = true;
			}

         Infinite = false;

      } else if (Delay) {

         send_best_move();
         Delay = false;
      }

   } else if (string_equal(string,"uci")) {

      ASSERT(!Searching);
      ASSERT(!Delay);

      send("id name neuroGrape " VERSION);
      send("id author Andrey Kotlarski");

      option_list();

      send("uciok");

   } else if (string_equal(string,"ucinewgame")) {

      if (!Searching && !Delay && Init) {
         trans_clear(Trans);
      } else {
         ASSERT(false);
      }
   }
}

// parse_go()

static void parse_go(char string[]) {

   const char * ptr;
   bool infinite, ponder;
   int depth, mate, movestogo;
   sint64 nodes;
   double binc, btime, movetime, winc, wtime;
   double time, inc;
   double time_max, alloc;
   int ThreadId;

   // init

   infinite = false;
   ponder = false;

   depth = -1;
   mate = -1;
   movestogo = -1;

   nodes = -1;

   binc = -1.0;
   btime = -1.0;
   movetime = -1.0;
   winc = -1.0;
   wtime = -1.0;

   // parse

   ptr = strtok(string," "); // skip "go"

   for (ptr = strtok(NULL," "); ptr != NULL; ptr = strtok(NULL," ")) {

      if (false) {

      } else if (string_equal(ptr,"binc")) {

         ptr = strtok(NULL," ");
         if (ptr == NULL) my_fatal("parse_go(): missing argument\n");

         binc = double(atoi(ptr)) / 1000.0;
         ASSERT(binc>=0.0);

      } else if (string_equal(ptr,"btime")) {

         ptr = strtok(NULL," ");
         if (ptr == NULL) my_fatal("parse_go(): missing argument\n");

         btime = double(atoi(ptr)) / 1000.0;
         ASSERT(btime>=0.0);

      } else if (string_equal(ptr,"depth")) {

         ptr = strtok(NULL," ");
         if (ptr == NULL) my_fatal("parse_go(): missing argument\n");

         depth = atoi(ptr);
         ASSERT(depth>=0);

      } else if (string_equal(ptr,"infinite")) {

         infinite = true;

      } else if (string_equal(ptr,"mate")) {

         ptr = strtok(NULL," ");
         if (ptr == NULL) my_fatal("parse_go(): missing argument\n");

         mate = atoi(ptr);
         ASSERT(mate>=0);

      } else if (string_equal(ptr,"movestogo")) {

         ptr = strtok(NULL," ");
         if (ptr == NULL) my_fatal("parse_go(): missing argument\n");

         movestogo = atoi(ptr);
         ASSERT(movestogo>=0);

      } else if (string_equal(ptr,"movetime")) {

         ptr = strtok(NULL," ");
         if (ptr == NULL) my_fatal("parse_go(): missing argument\n");

         movetime = double(atoi(ptr)) / 1000.0;
         ASSERT(movetime>=0.0);

      } else if (string_equal(ptr,"nodes")) {

         ptr = strtok(NULL," ");
         if (ptr == NULL) my_fatal("parse_go(): missing argument\n");

         nodes = my_atoll(ptr);
         ASSERT(nodes>=0);

      } else if (string_equal(ptr,"ponder")) {

         ponder = true;

      } else if (string_equal(ptr,"searchmoves")) {

         // dummy

      } else if (string_equal(ptr,"winc")) {

         ptr = strtok(NULL," ");
         if (ptr == NULL) my_fatal("parse_go(): missing argument\n");

         winc = double(atoi(ptr)) / 1000.0;
         ASSERT(winc>=0.0);

      } else if (string_equal(ptr,"wtime")) {

         ptr = strtok(NULL," ");
         if (ptr == NULL) my_fatal("parse_go(): missing argument\n");

         wtime = double(atoi(ptr)) / 1000.0;
         ASSERT(wtime>=0.0);
      }
   }

   // init

   search_clear();

   // depth limit

   // JAS
   int option_depth = 0;
   option_depth = option_get_int("Search Depth");
   if (option_depth > 0) {
   	  depth = option_depth;
   }
   // JAS end

   if (depth >= 0) {
      SearchInput->depth_is_limited = true;
      SearchInput->depth_limit = depth;
   } else if (mate >= 0) {
      SearchInput->depth_is_limited = true;
      SearchInput->depth_limit = mate * 2 - 1; // HACK: move -> ply
   }

   // time limit

   if (COLOUR_IS_WHITE(SearchInput->board->turn)) {
      time = wtime;
      inc = winc;
   } else {
      time = btime;
      inc = binc;
   }

   if (movestogo <= 0 || movestogo > 30) movestogo = 30; // HACK
   if (inc < 0.0) inc = 0.0;

   // JAS
   int option_movetime = 0;
   option_movetime = option_get_int("Search Time");
   if (option_movetime > 0) {
   	  movetime = option_movetime;
   }
   // JAS end

   if (movetime >= 0.0) {

      // fixed time

      SearchInput->time_is_limited = true;
      SearchInput->time_limit_1 = movetime * 5.0; // HACK to avoid early exit
      SearchInput->time_limit_2 = movetime;

   } else if (time >= 0.0) {

      // dynamic allocation

      time_max = time * 0.95 - 1.0;
      if (time_max < 0.0) time_max = 0.0;

      SearchInput->time_is_limited = true;

      alloc = (time_max + inc * double(movestogo-1)) / double(movestogo);
      alloc *= (option_get_bool("Ponder") ? PonderRatio : NormalRatio);
      if (alloc > time_max) alloc = time_max;
      SearchInput->time_limit_1 = alloc;

      alloc = (time_max + inc * double(movestogo-1)) * 0.5;
      if (alloc < SearchInput->time_limit_1) alloc = SearchInput->time_limit_1;
      if (alloc > time_max) alloc = time_max;
      SearchInput->time_limit_2 = alloc;
   }

   if (infinite || ponder) SearchInput->infinite = true;

   // search

   ASSERT(!Searching);
   ASSERT(!Delay);

   Searching = true;
   Infinite = infinite || ponder;
   Delay = false;

   search();
	for (ThreadId = 0; ThreadId < NumberThreads; ThreadId++){
		search_update_current(ThreadId);
	}

   ASSERT(Searching);
   ASSERT(!Delay);

   Searching = false;
   Delay = Infinite;

   if (!Delay) send_best_move();
}

// parse_position()

static void parse_position(char string[]) {

   const char * fen;
   char * moves;
   const char * ptr;
   char move_string[256];
   int move;
   undo_t undo[1];

   // init

   fen = strstr(string,"fen ");
   moves = strstr(string,"moves ");

   // start position

   if (fen != NULL) { // "fen" present

      if (moves != NULL) { // "moves" present
         ASSERT(moves>fen);
         moves[-1] = '\0'; // dirty, but so is UCI
      }

      board_from_fen(SearchInput->board,fen+4); // CHANGE ME

   } else {

      // HACK: assumes startpos

      board_from_fen(SearchInput->board,StartFen);
   }

   // moves

   if (moves != NULL) { // "moves" present

      ptr = moves + 6;

      while (*ptr != '\0') {

         move_string[0] = *ptr++;
         move_string[1] = *ptr++;
         move_string[2] = *ptr++;
         move_string[3] = *ptr++;

         if (*ptr == '\0' || *ptr == ' ') {
            move_string[4] = '\0';
         } else { // promote
            move_string[4] = *ptr++;
            move_string[5] = '\0';
         }

         move = move_from_string(move_string,SearchInput->board);

         move_do(SearchInput->board,move,undo);

         while (*ptr == ' ') ptr++;
      }
   }
}

// parse_setoption()

static void parse_setoption(char string[]) {

   const char * name;
   char * value;
	uint32 egbb_cache_size;
	char * egbb_path;

   // init

   name = strstr(string,"name ");
   value = strstr(string,"value ");

   if (name == NULL || value == NULL || name >= value) return; // ignore buttons

   value[-1] = '\0'; // HACK
   name += 5;
   value += 6;

   // update

   option_set(name,value);

	// update bitbases if needed

	if (my_string_equal(name,"Bitbases Path") || my_string_equal(name,"Bitbases Cache Size")){

		egbb_cache_size = (option_get_int("Bitbases Cache Size") * 1024 * 1024);
		egbb_path = (char *) option_get_string("Bitbases Path"); 
		egbb_is_loaded = LoadEgbbLibrary(egbb_path,egbb_cache_size);
		if (!egbb_is_loaded)
			printf("EgbbProbe not Loaded!\n");
	}

   // update transposition-table size if needed

   if (Init && my_string_equal(name,"Hash")) { // Init => already allocated

      ASSERT(!Searching);

      if (option_get_int("Hash") >= 4) {
         trans_free(Trans);
         trans_alloc(Trans);
      }
   }

   if (my_string_equal(name,"Threads")) {
      init_threads(true);
   }
}

// send_best_move()

static void send_best_move() {

   double time, speed, cpu;
   sint64 node_nb;
   char move_string[256];
   char ponder_string[256];
   int move;
   int ThreadId, bestThreadId;
   mv_t * pv;

   // info

   // HACK: should be in search.cpp

   time = SearchCurrent[0]->time;
   speed = 0;
   for (ThreadId = 0; ThreadId < NumberThreads; ThreadId++){
		speed += SearchCurrent[ThreadId]->speed;
   }
   cpu = SearchCurrent[0]->cpu;

   node_nb = 0;
   for (ThreadId = 0; ThreadId < NumberThreads; ThreadId++){
		node_nb += SearchCurrent[ThreadId]->node_nb;
   }

   send("info time %.0f nodes " S64_FORMAT " nps %.0f cpuload %.0f",time*1000.0,node_nb,speed,cpu*1000.0);

   trans_stats(Trans);

   // best move

   bestThreadId = 0;
   for (ThreadId = 1; ThreadId < NumberThreads; ThreadId++){
	   if (SearchBest[bestThreadId][0].depth < SearchBest[ThreadId][0].depth ||
		     (SearchBest[bestThreadId][0].depth == SearchBest[ThreadId][0].depth &&
			  SearchBest[bestThreadId][0].value < SearchBest[ThreadId][0].value)){
		   bestThreadId = ThreadId;
	   }
   }
   move = SearchBest[bestThreadId][0].move;
   pv = SearchBest[bestThreadId][0].pv;

   move_to_string(move,move_string,256);

   /* move_to_string(SearchBest[1][0].move,ponder_string,256);
   if (SearchBest[0][0].move != SearchBest[1][0].move){
	   FILE *fp;
	   fp=fopen("toga.log","a+");
	   fprintf(fp,"move: %s depth: %d value: %d move: %s depth: %d value: %d\n",move_string,SearchBest[0][0].depth, SearchBest[0][0].value, ponder_string,SearchBest[1][0].depth, SearchBest[1][0].value);
	   fclose(fp); 
   }*/

   if (pv[0] == move && move_is_ok(pv[1])) {
      move_to_string(pv[1],ponder_string,256);
      send("bestmove %s ponder %s",move_string,ponder_string);
   } else {
      send("bestmove %s",move_string);
   }
}

// get()

void get(char string[], int size) {

   ASSERT(string!=NULL);
   ASSERT(size>=65536);

   if (!my_file_read_line(stdin,string,size)) { // EOF
      exit(EXIT_SUCCESS);
   }
}

// send()

void send(const char format[], ...) {

   va_list arg_list;
   char string[4096];

   ASSERT(format!=NULL);

   va_start(arg_list,format);
   vsprintf(string,format,arg_list);
   va_end(arg_list);

   fprintf(stdout,"%s\n",string);
}

// string_equal()

static bool string_equal(const char s1[], const char s2[]) {

   ASSERT(s1!=NULL);
   ASSERT(s2!=NULL);

   return strcmp(s1,s2) == 0;
}

// string_start_with()

static bool string_start_with(const char s1[], const char s2[]) {

   ASSERT(s1!=NULL);
   ASSERT(s2!=NULL);

   return strstr(s1,s2) == s1;
}

// end of protocol.cpp

