// search_full.cpp

// includes

#include "pawn.h"
#include "attack.h"
#include "board.h"
#include "colour.h"
#include "eval.h"
#include "list.h"
#include "move.h"
#include "move_check.h"
#include "move_do.h"
#include "option.h"
#include "piece.h"
#include "pst.h"
#include "pv.h"
#include "recog.h"
#include "search.h"
#include "search_full.h"
#include "see.h"
#include "sort.h"
#include "trans.h"
#include "util.h"
#include "value.h"
#include "probe.h"

#define ABS(x) ((x)<0?-(x):(x))
#define MIN(X, Y)  ((X) < (Y) ? (X) : (Y))
#define R_adpt(max_pieces, depth, Reduction) (Reduction + ((depth) > (9 + ((max_pieces<3) ? 2 : 0))))
#define usable_egbb_value(probe_score,alpha,beta) (((beta > -ValueEvalInf || probe_score >= 0) \
		 && (alpha < ValueEvalInf || probe_score <= 0)) || probe_score < -ValueEvalInf)

// constants and variables

static const int WindowSize = 20;

// main search

static const bool UseDistancePruning = true;

// transposition table

static const bool UseTrans = true;
static const int TransDepth = 1;
static const bool UseExact = true;

// null move

static bool UseNull = true;
static const int NullDepth = 2;
static int NullReduction = 3;

static bool UseVer = true;
static int VerReduction = 5;

// move ordering

static const bool UseIID = true;
static const int IIDDepth = 3;
static const int IIDReduction = 2;

// extensions

static const bool ExtendSingleReply = true;

// history pruning

static bool UseHistory = true;
static const int HistoryDepth = 3;
static const int HistoryMoveNb = 5;
static const int HistoryPVMoveNb = 10; 
static int HistoryValue = 9830; // 60%

// futility pruning

static bool UseFutility = true;
static const int FutilityMarginBase = 300;
static const int FutilityMarginStep = 50;

// quiescence search

static bool UseDelta = true;
static int DeltaMargin = 50;

static int CheckDepth[MaxThreads];

// misc

static const int NodeAll = -1;
static const int NodePV  =  0;
static const int NodeCut = +1;

// macros

#define NODE_NEXT(type)     (type == NodePV ? NodePV : NodeAll)
#define DEPTH_MATCH(d1,d2) ((d1)>=(d2))

// prototypes

static int  full_root            (list_t * list, board_t * board, int alpha, int beta, int depth, int height, int search_type, int ThreadId);

static int  full_search          (board_t * board, int alpha, int beta, int depth, int height, mv_t pv[], int node_type, bool extended, int ThreadId);
static int  full_no_null         (board_t * board, int alpha, int beta, int depth, int height, mv_t pv[], int node_type, int trans_move, int * best_move, bool extended, int ThreadId);

static int  full_quiescence      (board_t * board, int alpha, int beta, int depth, int height, mv_t pv[], int ThreadId);

static int  full_new_depth       (int depth, int move, board_t * board, bool single_reply, bool in_pv, int height, bool extended, bool * cap_extended, int ThreadId);

static bool do_null              (const board_t * board);
static bool do_ver               (const board_t * board);

static void pv_fill              (const mv_t pv[], board_t * board);

static bool move_is_dangerous    (int move, const board_t * board);
static bool capture_is_dangerous (int move, const board_t * board);

static bool simple_stalemate     (const board_t * board);

static bool passed_pawn_move     (int move, const board_t * board);
static bool is_passed		     (const board_t * board, int to);

// functions

// search_full_init()

void search_full_init(list_t * list, board_t * board, int ThreadId) {

   const char * string;
   int trans_move, trans_depth, trans_flags, trans_value;
   entry_t * found_entry;
   
   // init

   CheckDepth[ThreadId] = 0;

   // standard sort

   list_note(list);
   list_sort(list);

   // basic sort

   trans_move = MoveNone;
   if (UseTrans) trans_retrieve(Trans,&found_entry,board->key,&trans_move,&trans_depth,&trans_flags,&trans_value);
   note_moves(list,board,0,trans_move,ThreadId);
   list_sort(list);
}

// search_full_root()

int search_full_root(list_t * list, board_t * board, int depth, int search_type, int ThreadId) {

   int value, a, b;

   if (depth < 3) {
      a = -ValueInf;
      b = +ValueInf;
   } else {
	  if (SearchRoot[ThreadId]->last_value == ValueDraw) {
         a = SearchRoot[ThreadId]->last_value - 1;
		 b = SearchRoot[ThreadId]->last_value + 1;
	  } else {
         a = SearchRoot[ThreadId]->last_value - WindowSize;
		 b = SearchRoot[ThreadId]->last_value + WindowSize;
	  }
   }

   value = full_root(list,board,a,b,depth,0,search_type, ThreadId);
   return value;
}

// full_root()

static int full_root(list_t * list, board_t * board, int alpha, int beta, int depth, int height, int search_type, int ThreadId) {

   int old_alpha;
   int value, best_value[MultiPVMax];
   int i, move, j;
   int new_depth;
   undo_t undo[1];
   mv_t new_pv[HeightMax];
   bool found;
   bool cap_extended;
   bool reduced;

   // init

   SearchCurrent[ThreadId]->node_nb++;
   SearchInfo[ThreadId]->check_nb--;

   if (SearchCurrent[ThreadId]->multipv == 0)
	  for (i = 0; i < LIST_SIZE(list); i++) list->value[i] = ValueNone;

   old_alpha = alpha;
   best_value[SearchCurrent[ThreadId]->multipv] = ValueNone;

   // move loop

   for (i = 0; i < LIST_SIZE(list); i++) {

      move = LIST_MOVE(list,i);

	  if (SearchCurrent[ThreadId]->multipv > 0) {
		 found = false;
		 for (j = 0; j < SearchCurrent[ThreadId]->multipv; j++) {
			if (SearchBest[ThreadId][j].pv[0] == move) {
			   found = true;
			   break;
			}
		 }
		 if (found == true) continue;
	  }
	  
      SearchRoot[ThreadId]->depth = depth;
      SearchRoot[ThreadId]->move = move;
      SearchRoot[ThreadId]->move_pos = i;
      SearchRoot[ThreadId]->move_nb = LIST_SIZE(list);

      search_update_root(ThreadId);

      new_depth = full_new_depth(depth,move,board,board_is_check(board)&&LIST_SIZE(list)==1,true, height, false, &cap_extended,ThreadId);

	  reduced = false;
	  if (i > HistoryPVMoveNb && search_type == SearchNormal && depth >= 3 
		 && new_depth < depth && !move_is_tactical(move,board)) {
         new_depth--;
		 reduced = true;
	  }

      move_do(board,move,undo);

      if (search_type == SearchShort || best_value[SearchCurrent[ThreadId]->multipv] == ValueNone) { // first move
		 value = -full_search(board,-beta,-alpha,new_depth,height+1,new_pv,NodePV,cap_extended,ThreadId);
		 if (value <= alpha){ // research
			value = -full_search(board,-alpha,ValueInf,new_depth,height+1,new_pv,NodePV,cap_extended,ThreadId);
			old_alpha = alpha = -ValueInf;
		 } else if (value >= beta){
			value = -full_search(board,-ValueInf,-beta,new_depth,height+1,new_pv,NodePV,cap_extended,ThreadId);
			beta = +ValueInf;
		 }
      } else { // other moves
         value = -full_search(board,-alpha-1,-alpha,new_depth,height+1,new_pv,NodeCut,cap_extended,ThreadId);
         if (value > alpha) { // && value < beta
            SearchRoot[ThreadId]->change = true;
            SearchRoot[ThreadId]->easy = false;
            SearchRoot[ThreadId]->flag = false;
			if (reduced) new_depth++;
			value = -full_search(board,-ValueInf,-alpha,new_depth,height+1,new_pv,NodePV,cap_extended,ThreadId);
			if (value >= beta) {
               beta = +ValueInf;
			}
         }
      }

      move_undo(board,move,undo);

	  list->value[i] = value;

      if (value > best_value[SearchCurrent[ThreadId]->multipv] && (best_value[SearchCurrent[ThreadId]->multipv] == ValueNone || value > alpha)) {

         SearchBest[ThreadId][SearchCurrent[ThreadId]->multipv].move = move;
		 SearchBest[ThreadId][SearchCurrent[ThreadId]->multipv].value = value;
         if (value <= alpha) { // upper bound
            SearchBest[ThreadId][SearchCurrent[ThreadId]->multipv].flags = SearchUpper;
         } else if (value >= beta) { // lower bound
            SearchBest[ThreadId][SearchCurrent[ThreadId]->multipv].flags = SearchLower;
         } else { // alpha < value < beta => exact value
            SearchBest[ThreadId][SearchCurrent[ThreadId]->multipv].flags = SearchExact;
         }
         SearchBest[ThreadId][SearchCurrent[ThreadId]->multipv].depth = depth;
         pv_cat(SearchBest[ThreadId][SearchCurrent[ThreadId]->multipv].pv,new_pv,move);

         search_update_best(ThreadId);
      }

      if (value > best_value[SearchCurrent[ThreadId]->multipv]) {
         best_value[SearchCurrent[ThreadId]->multipv] = value;
         if (value > alpha) {
            if (search_type == SearchNormal) alpha = value;
         }
      }
   }

   list_sort(list);

   ASSERT(SearchBest[ThreadId]->move==LIST_MOVE(list,0));
   ASSERT(SearchBest[ThreadId]->value==best_value);

   if (UseTrans && best_value[SearchCurrent[ThreadId]->multipv] > old_alpha && best_value[SearchCurrent[ThreadId]->multipv] < beta) {
      pv_fill(SearchBest[ThreadId][SearchCurrent[ThreadId]->multipv].pv,board);
   }

   good_move(SearchBest[ThreadId]->move,board,depth,height,ThreadId);

   return best_value[SearchCurrent[ThreadId]->multipv];
}

// full_search()

static int full_search(board_t * board, int alpha, int beta, int depth, int height, mv_t pv[], int node_type, bool extended, int ThreadId) {

   bool in_check;
   bool single_reply;
   bool good_cap;
   int trans_move, trans_depth, trans_flags, trans_value;
   int old_alpha;
   int value, best_value;
   int move, best_move;
   int new_depth;
   int played_nb;
   int i;
   int opt_value;
   bool reduced, cap_extended;
   attack_t attack[1];
   sort_t sort[1];
   undo_t undo[1];
   mv_t new_pv[HeightMax];
   mv_t played[256];
   int FutilityMargin;
   int probe_score, probe_depth;
   entry_t * found_entry;

   // horizon?

   if (depth <= 0) {
	  if (node_type == NodePV) CheckDepth[ThreadId] = -1;
	  else CheckDepth[ThreadId] = 0;
	  return full_quiescence(board,alpha,beta,0,height,pv,ThreadId);
   }

   // init

   SearchCurrent[ThreadId]->node_nb++;
   SearchInfo[ThreadId]->check_nb--;
   PV_CLEAR(pv);

   if (height > SearchCurrent[ThreadId]->max_depth) SearchCurrent[ThreadId]->max_depth = height;

   if (SearchInfo[ThreadId]->check_nb <= 0) {
      SearchInfo[ThreadId]->check_nb += SearchInfo[ThreadId]->check_inc;
      search_check(ThreadId);
   }

   // draw?

   if (board_is_repetition(board)) return ValueDraw;

   // mate-distance pruning

   if (UseDistancePruning) {

      // lower bound

      value = VALUE_MATE(height+2); // does not work if the current position is mate
      if (value > alpha && board_is_mate(board)) value = VALUE_MATE(height);

      if (value > alpha) {
         alpha = value;
         if (value >= beta) return value;
      }

      // upper bound

      value = -VALUE_MATE(height+1);

      if (value < beta) {
         beta = value;
         if (value <= alpha) return value;
      }
   }

   if (recog_draw(board,ThreadId)) return ValueDraw;

   bool can_probe = egbb_is_loaded && board->piece_nb <= 5 
	  && (board->cap_sq != SquareNone || PIECE_IS_PAWN(board->moving_piece) // progress
	  ||  height == (2 * SearchCurrent[ThreadId]->act_iteration) / 3); // only if root 5-men or less

   if (can_probe && board->piece_nb <= 4 && probe_bitbases(board, probe_score)) {
	  probe_score = value_from_trans(probe_score,height);
	  if (usable_egbb_value(probe_score,alpha,beta)) 
		 return probe_score;
   }

   // transposition table

   trans_move = MoveNone;

   if (UseTrans && depth >= TransDepth) {
      if (trans_retrieve(Trans,&found_entry,board->key,&trans_move,&trans_depth,&trans_flags,&trans_value)) {
	     if (node_type != NodePV) {
            if (trans_depth < depth) {
			   if (trans_value < -ValueEvalInf && TRANS_IS_UPPER(trans_flags)) {
				  trans_depth = depth;
				  trans_flags = TransUpper;
			   } else if (trans_value > +ValueEvalInf && TRANS_IS_LOWER(trans_flags)) {
				  trans_depth = depth;
				  trans_flags = TransLower;
			   }
		    }

            if (trans_flags == TransEGBB && board->piece_nb == 5 && usable_egbb_value(trans_value,alpha,beta)) 
			return value_from_trans(trans_value,height);

			if (trans_depth >= depth) {
               trans_value = value_from_trans(trans_value,height);
               if ((UseExact && TRANS_IS_EXACT(trans_flags))
				  || (TRANS_IS_LOWER(trans_flags) && trans_value >= beta)
				  || (TRANS_IS_UPPER(trans_flags) && trans_value <= alpha)) {
				  return trans_value;
			   } 
			}
		 }
	  }
   }

   if (can_probe && board->piece_nb == 5 && probe_bitbases(board,probe_score)) {
	  if (probe_score > -ValueEvalInf) {
		 trans_store(Trans,board->key,MoveNone,depth,TransEGBB,probe_score);
		 if (usable_egbb_value(probe_score,alpha,beta)) 
			return value_from_trans(probe_score,height);
	  }
	  else return value_from_trans(probe_score,height);
   }

   // height limit

   if (height >= HeightMax-1) return eval(board, alpha, beta, ThreadId);

   // more init

   old_alpha = alpha;
   best_value = ValueNone;
   best_move = MoveNone;
   played_nb = 0;
   
   attack_set(attack,board);
   in_check = ATTACK_IN_CHECK(attack);

   // null-move pruning

   if (UseNull && depth >= NullDepth && node_type != NodePV) {
      if (!in_check && !value_is_mate(beta) && do_null(board)) {

         // null-move search
		 
         new_depth = depth - NullReduction - 1;
		 
	     move_do_null(board,undo);
         value = -full_search(board,-beta,-beta+1,new_depth,height+1,new_pv,NODE_NEXT(node_type),false,ThreadId);
         move_undo_null(board,undo);

         // verification search

         if (UseVer && depth > VerReduction) {

            if (value >= beta) {

               new_depth = depth - VerReduction;

               value = full_no_null(board,alpha,beta,new_depth,height,new_pv,NodeCut,trans_move,&move,false,ThreadId);

               if (value >= beta) {
                  played[played_nb++] = move;
                  best_move = move;
				  best_value = value;
                  pv_copy(pv,new_pv);
                  goto cut;
               }
            }
         }

         // pruning

         if (value >= beta) {

            if (value > +ValueEvalInf) value = +ValueEvalInf; // do not return unproven mates

            best_move = MoveNone;
            best_value = value;
            goto cut;
         }
	  }
   }

   // Internal Iterative Deepening
   
   if (UseIID && depth >= IIDDepth && node_type == NodePV && trans_move == MoveNone) {

	  new_depth = MIN(depth - IIDReduction,depth/2);
      ASSERT(new_depth>0);

      value = full_search(board,alpha,beta,new_depth,height,new_pv,node_type,false,ThreadId);
      if (value <= alpha) value = full_search(board,-ValueInf,beta,new_depth,height,new_pv,node_type,false,ThreadId);

      trans_move = new_pv[0];
   }

   // move generation

   sort_init(sort,board,attack,depth,height,trans_move,ThreadId);

   single_reply = false;
   if (in_check && LIST_SIZE(sort->list) == 1) single_reply = true; // HACK

   // move loop

   opt_value = +ValueInf;
   good_cap = true;
   
   while ((move=sort_next(sort,ThreadId)) != MoveNone) {

	  // extensions

      new_depth = full_new_depth(depth,move,board,single_reply,node_type==NodePV, height, extended, &cap_extended, ThreadId);
	  
      // history pruning

      value = sort->value; // history score
	  if (!in_check && depth <= 4 && node_type != NodePV && new_depth < depth 
		  && value < HistoryValue/(depth/3+1) && played_nb >= 1+depth 
		  && !move_is_check(move,board) && !move_is_dangerous(move,board)) { 
			continue;
	  }

	  // futility pruning

	  if (UseFutility && node_type != NodePV && depth <= 3) {
		  
         if (!in_check && new_depth < depth && !move_is_tactical(move,board) 
			&& !move_is_check(move,board) && !move_is_dangerous(move,board)) {

            // optimistic evaluation

            if (opt_value == +ValueInf) {
				if (depth>=2) FutilityMargin = FutilityMarginBase + (depth - 2) * FutilityMarginStep;
				else FutilityMargin = DeltaMargin;
				opt_value = eval(board,32768,-32768,ThreadId) + FutilityMargin; // no lazy evaluation
            }

            value = opt_value;

            // pruning

            if (value <= alpha) {

               if (value > best_value) {
                  best_value = value;
                  PV_CLEAR(pv);
               }

               continue;
            }
         }
      } 
	  
	  reduced = false;

	  if (good_cap && !move_is_tactical(move,board)){
		 good_cap = false;
	  }

	  if (UseHistory) {
		 if (!in_check && new_depth < depth && played_nb >= HistoryMoveNb 
		    && depth >= HistoryDepth && !move_is_check(move,board) && !move_is_dangerous(move,board)) {
			if (!good_cap) {
			   if (node_type == NodeAll) {
				  new_depth--;
				  reduced = true;
			   } else {
				  if (played_nb>= HistoryPVMoveNb) {
					 new_depth--;
					 reduced = true;
				  }
			   }
			}
		 }
	  }

	  // recursive search

	  move_do(board,move,undo);

      if (node_type != NodePV || best_value == ValueNone) { // first move
		 value = -full_search(board,-beta,-alpha,new_depth,height+1,new_pv,NODE_NEXT(node_type),cap_extended,ThreadId);
		 if (value > alpha && reduced) {
            new_depth++;
            value = -full_search(board,-beta,-alpha,new_depth,height+1,new_pv,NODE_NEXT(node_type),cap_extended,ThreadId);
		 }
      } else { // other moves
		 value = -full_search(board,-alpha-1,-alpha,new_depth,height+1,new_pv,NodeCut,cap_extended,ThreadId);
         if (value > alpha) {
		    if (reduced) new_depth++;
			value = -full_search(board,-beta,-alpha,new_depth,height+1,new_pv,NodePV,cap_extended,ThreadId);
         }
      }

      move_undo(board,move,undo);

      played[played_nb++] = move;
	  
      if (value > best_value) {
         best_value = value;
         pv_cat(pv,new_pv,move);
         if (value > alpha) {
            alpha = value;
            best_move = move;
			if (value >= beta){ 
				goto cut;
			}
         }
      }
   }

   // ALL node

   if (best_value == ValueNone) { // no legal move
      if (in_check) {
         ASSERT(board_is_mate(board));
         return VALUE_MATE(height);
      } else {
         ASSERT(board_is_stalemate(board));
         return ValueDraw;
      }
   }

cut:

   ASSERT(value_is_ok(best_value));

   // move ordering

   if (best_move != MoveNone) {

      good_move(best_move,board,depth,height,ThreadId);

      if (best_value >= beta && !move_is_tactical(best_move,board)) {

		 ASSERT(played_nb>0&&played[played_nb-1]==best_move);

	 	 for (i = 0; i < played_nb-1; i++) {
			move = played[i];
			ASSERT(move!=best_move);
			history_bad(move,board,ThreadId);
		 }
	 
		 history_good(best_move,board,ThreadId);
		
      }
   }

   // transposition table

   if (UseTrans && depth >= TransDepth) {

      trans_move = best_move;
      trans_depth = depth;
      trans_flags = TransUnknown;
      if (best_value > old_alpha) trans_flags |= TransLower;
      if (best_value < beta) trans_flags |= TransUpper;
      trans_value = value_to_trans(best_value,height);

      trans_store(Trans,board->key,trans_move,trans_depth,trans_flags,trans_value);

   }

   return best_value;
}

// full_no_null()

static int full_no_null(board_t * board, int alpha, int beta, int depth, int height, mv_t pv[], int node_type, int trans_move, int * best_move, bool extended, int ThreadId) {

   int value, best_value;
   int move;
   int new_depth;
   attack_t attack[1];
   sort_t sort[1];
   undo_t undo[1];
   mv_t new_pv[HeightMax];
   bool cap_extended;

   // init

   SearchCurrent[ThreadId]->node_nb++;
   SearchInfo[ThreadId]->check_nb--;
   PV_CLEAR(pv);

   if (height > SearchCurrent[ThreadId]->max_depth) SearchCurrent[ThreadId]->max_depth = height;

   if (SearchInfo[ThreadId]->check_nb <= 0) {
      SearchInfo[ThreadId]->check_nb += SearchInfo[ThreadId]->check_inc;
      search_check(ThreadId);
   }

   attack_set(attack,board);

   *best_move = MoveNone;
   best_value = ValueNone;

   // move loop

   sort_init(sort,board,attack,depth,height,trans_move,ThreadId);

   while ((move=sort_next(sort,ThreadId)) != MoveNone) {

	  new_depth = full_new_depth(depth,move,board,false,false,height,extended,&cap_extended,ThreadId);

      move_do(board,move,undo);
      value = -full_search(board,-beta,-alpha,new_depth,height+1,new_pv,NODE_NEXT(node_type),cap_extended,ThreadId);
      move_undo(board,move,undo);

      if (value > best_value) {
         best_value = value;
         pv_cat(pv,new_pv,move);
         if (value > alpha) {
            alpha = value;
            *best_move = move;
			if (value >= beta) goto cut;
         }
      }
   }

   // ALL node

   if (best_value == ValueNone) { // no legal move => stalemate
      ASSERT(board_is_stalemate(board));
      best_value = ValueDraw;
   }

cut:

   ASSERT(value_is_ok(best_value));

   return best_value;
}

// full_quiescence()

static int full_quiescence(board_t * board, int alpha, int beta, int depth, int height, mv_t pv[], int ThreadId) {

   bool in_check;
   int old_alpha;
   int value, best_value;
   int best_move;
   int move;
   int opt_value;
   attack_t attack[1];
   sort_t sort[1];
   undo_t undo[1];
   mv_t new_pv[HeightMax];
   int probe_score, probe_depth;

   // init

   SearchCurrent[ThreadId]->node_nb++;
   SearchInfo[ThreadId]->check_nb--;
   PV_CLEAR(pv);

   if (height > SearchCurrent[ThreadId]->max_depth) SearchCurrent[ThreadId]->max_depth = height;

   if (SearchInfo[ThreadId]->check_nb <= 0) {
      SearchInfo[ThreadId]->check_nb += SearchInfo[ThreadId]->check_inc;
      search_check(ThreadId);
   }

   // draw?

   if (board_is_repetition(board)) return ValueDraw;
   if (recog_draw(board,ThreadId)) return ValueDraw;

   // mate-distance pruning

   if (UseDistancePruning) {

      // lower bound

      value = VALUE_MATE(height+2);
      if (value > alpha && board_is_mate(board)) value = VALUE_MATE(height);

      if (value > alpha) {
         alpha = value;
         if (value >= beta) return value;
      }

      // upper bound

      value = -VALUE_MATE(height+1);

      if (value < beta) {
         beta = value;
         if (value <= alpha) return value;
      }
   }

   // more init

   attack_set(attack,board);
   in_check = ATTACK_IN_CHECK(attack);

   if (in_check) depth++;

   // height limit

   if (height >= HeightMax-1) return eval(board, alpha, beta, ThreadId);

   // more init

   old_alpha = alpha;
   best_value = ValueNone;
   best_move = MoveNone;

   opt_value = +ValueInf;

   if (!in_check) {

      // lone-king stalemate?

      if (simple_stalemate(board)) return ValueDraw;

      // stand pat

      value = eval(board, alpha, beta, ThreadId);

      best_value = value;
      if (value > alpha) {
         alpha = value;
         if (value >= beta) goto cut;
      }

      if (UseDelta) opt_value = value + DeltaMargin;
   }

   sort_init_qs(sort,board,attack,depth>=CheckDepth[ThreadId]);

   while ((move=sort_next_qs(sort)) != MoveNone) {

	  // delta pruning

      if (UseDelta && beta == old_alpha+1) {

         if (!in_check && !move_is_check(move,board) && !capture_is_dangerous(move,board)) {

            ASSERT(move_is_tactical(move,board));

            // optimistic evaluation

            value = opt_value;

            int to = MOVE_TO(move);
            int capture = board->square[to];

            if (capture != Empty) {
               value += VALUE_PIECE(capture);
            } else if (MOVE_IS_EN_PASSANT(move)) {
               value += ValuePawn;
            }

            if (MOVE_IS_PROMOTE(move)) value += ValueQueen - ValuePawn;

			if (PIECE_IS_BISHOP(capture)) {
               if (board->turn == White && board->number[BlackBishop12] >= 2) value += 50;
			   if (board->turn == Black && board->number[WhiteBishop12] >= 2) value += 50;
			}

            // pruning

            if (value <= alpha) {

               if (value > best_value) {
                  best_value = value;
                  PV_CLEAR(pv);
               }

               continue;
            }
         }
      }

      move_do(board,move,undo);
      value = -full_quiescence(board,-beta,-alpha,depth-1,height+1,new_pv,ThreadId);
      move_undo(board,move,undo);

      if (value > best_value) {
         best_value = value;
         pv_cat(pv,new_pv,move);
         if (value > alpha) {
            alpha = value;
            best_move = move;
			if (value >= beta) goto cut;
         }
      }
   }

   // ALL node

   if (best_value == ValueNone) {
      ASSERT(board_is_mate(board));
      return VALUE_MATE(height);
   }

cut:

   ASSERT(value_is_ok(best_value));

   return best_value;
}

// full_new_depth()

static int full_new_depth(int depth, int move, board_t * board, bool single_reply, bool in_pv, int height, bool extended, bool * cap_extended, int ThreadId) {
   *cap_extended = false;
   int to = MOVE_TO(move);
   int capture = board->square[to];
   int piece_nb = board->piece_size[White] + board->piece_size[Black];
   
   if (in_pv && piece_nb <= 5 && capture != Empty && !PIECE_IS_PAWN(capture)) return depth;
   if (single_reply && ExtendSingleReply) return depth;
   if (move_is_check(move,board) && (in_pv || see_move(move,board) >= -100)) return depth;
   if (in_pv && move_is_dangerous(move,board)) return depth;

   if (in_pv && capture != Empty && !extended && see_move(move,board) >= -100) {
	  *cap_extended = true;
	  return depth;
   }
   return depth - 1;
}

// do_null()

static bool do_null(const board_t * board) {

   ASSERT(board!=NULL);

   // use null move if the side-to-move has at least one piece

   return board->piece_size[board->turn] >= 2; // king + one piece
}

// do_ver()

static bool do_ver(const board_t * board) {

   ASSERT(board!=NULL);

   // use verification if the side-to-move has at most one piece

   return board->piece_size[board->turn] <= 3; // king + one piece
}

// pv_fill()

static void pv_fill(const mv_t pv[], board_t * board) {

   int move;
   int trans_move, trans_depth;
   undo_t undo[1];

   ASSERT(pv!=NULL);
   ASSERT(board!=NULL);

   ASSERT(UseTrans);

   move = *pv;

   if (move != MoveNone && move != MoveNull) {

      move_do(board,move,undo);
      pv_fill(pv+1,board);
      move_undo(board,move,undo);

      trans_move = move;
      trans_depth = -127; // HACK
      
      trans_store(Trans,board->key,trans_move,trans_depth,TransUnknown,-ValueInf);
   }
}

// move_is_dangerous()

static bool move_is_dangerous(int move, const board_t * board) {

   int piece;

   ASSERT(move_is_ok(move));
   ASSERT(board!=NULL);

   ASSERT(!move_is_tactical(move,board));

   piece = MOVE_PIECE(move,board);

   if (PIECE_IS_PAWN(piece) && is_passed(board,MOVE_TO(move))) return true;

   return false;
}

// capture_is_dangerous()

static bool capture_is_dangerous(int move, const board_t * board) {

   int piece, capture;

   ASSERT(move_is_ok(move));
   ASSERT(board!=NULL);

   ASSERT(move_is_tactical(move,board));

   piece = MOVE_PIECE(move,board);

   if (PIECE_IS_PAWN(piece)
    && PAWN_RANK(MOVE_TO(move),board->turn) >= Rank7) {
      return true;
   }

   capture = move_capture(move,board);

   if (PIECE_IS_QUEEN(capture)) return true;

   if (PIECE_IS_PAWN(capture)
    && PAWN_RANK(MOVE_TO(move),board->turn) <= Rank2) {
      return true;
   }

   return false;
}

// simple_stalemate()

static bool simple_stalemate(const board_t * board) {

   int me, opp;
   int king;
   int opp_flag;
   int from, to;
   int capture;
   const inc_t * inc_ptr;
   int inc;

   ASSERT(board!=NULL);

   ASSERT(board_is_legal(board));
   ASSERT(!board_is_check(board));

   // lone king?

   me = board->turn;
   if (board->piece_size[me] != 1 || board->pawn_size[me] != 0) return false; // no

   // king in a corner?

   king = KING_POS(board,me);
   if (king != A1 && king != H1 && king != A8 && king != H8) return false; // no

   // init

   opp = COLOUR_OPP(me);
   opp_flag = COLOUR_FLAG(opp);

   // king can move?

   from = king;

   for (inc_ptr = KingInc; (inc=*inc_ptr) != IncNone; inc_ptr++) {
      to = from + inc;
      capture = board->square[to];
      if (capture == Empty || FLAG_IS(capture,opp_flag)) {
         if (!is_attacked(board,to,opp)) return false; // legal king move
      }
   }

   // no legal move

   ASSERT(board_is_stalemate((board_t*)board));

   return true;
}

static bool is_passed(const board_t * board, int to) { 

   int t2; 
   int me, opp;
   int file, rank;

   me = board->turn; 
   opp = COLOUR_OPP(me);
   file = SQUARE_FILE(to);
   rank = PAWN_RANK(to,me);
 
   t2 = board->pawn_file[me][file] | BitRev[board->pawn_file[opp][file]]; 

   // passed pawns 
   if ((t2 & BitGT[rank]) == 0) { 
 
    if (((BitRev[board->pawn_file[opp][file-1]] | BitRev[board->pawn_file[opp][file+1]]) & BitGT[rank]) == 0) { 
        return true;

       } 
   } 

   return false;
 
}

// end of search_full.cpp

