/*
** Copyright 2014 Guillaume Filion, Eduard Valera Zorita and Pol Cusco.
**
** File authors:
**  Guillaume Filion     (guillaume.filion@gmail.com)
**  Eduard Valera Zorita (ezorita@mit.edu)
**
** License: 
**  This program is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
**
**  This program is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with this program.  If not, see <http://www.gnu.org/licenses/>.
**
*/

#include "starcode.h"
#include "starcode-private.h"

//    Global variables    //
static FILE     * OUTPUTF1      = NULL;           // output file 1
static format_t   FORMAT        = UNSET;          // input format

int
starcode
(
   FILE *inputf1,
   FILE *inputf2,
   FILE *outputf1,
         int tau,
   const int verbose,
         int thrmax
)
{

   OUTPUTF1 = outputf1;

   if (verbose) {
      fprintf(stderr, "running starcode with %d thread%s\n",
           thrmax, thrmax > 1 ? "s" : "");
      fprintf(stderr, "reading input files\n");
   }
   gstack_t *uSQ = read_file(inputf1, inputf2, verbose);
   if (uSQ == NULL || uSQ->nitems < 1) {
      fprintf(stderr, "input file empty\n");
      return 1;
   }
   
   // Sort/reduce.
   if (verbose) fprintf(stderr, "sorting\n");
   uSQ->nitems = seqsort((useq_t **) uSQ->items, uSQ->nitems, thrmax);

   // Get number of tries.
   int ntries = 3 * thrmax + (thrmax % 2 == 0);
   if (uSQ->nitems < ntries) {
      ntries = 1;
      thrmax = 1;
   }
 
   // Pad sequences (and return the median size).
   // Compute 'tau' from it in "auto" mode.
   int med = -1;
   int height = pad_useq(uSQ, &med);
   if (tau < 0) {
      tau = med > 160 ? 8 : 2 + med/30;
      if (verbose) {
         fprintf(stderr, "setting dist to %d\n", tau);
      }
   }

   // Make multithreading plan.
   mtplan_t *mtplan = plan_mt(tau, height, med, ntries, uSQ);

   // Run the query.
   run_plan(mtplan, verbose, thrmax);
   if (verbose) fprintf(stderr, "progress: 100.00%%\n");

   // Do not free anything.
   OUTPUTF1 = NULL;
   return 0;

}

void
run_plan
(
   mtplan_t *mtplan,
   const int verbose,
   const int thrmax
)
{
   // Count total number of jobs.
   int njobs = mtplan->ntries * (mtplan->ntries+1) / 2;

   // Thread Scheduler
   int triedone = 0;
   int idx = -1;

   while (triedone < mtplan->ntries) { 
      // Cycle through the tries in turn.
      idx = (idx+1) % mtplan->ntries;
      mttrie_t *mttrie = mtplan->tries + idx;
      pthread_mutex_lock(mtplan->mutex);     

      // Check whether trie is idle and there are available threads.
      if (mttrie->flag == TRIE_FREE && mtplan->active < thrmax) {

         // No more jobs on this trie.
         if (mttrie->currentjob == mttrie->njobs) {
            mttrie->flag = TRIE_DONE;
            triedone++;
         }

         // Some more jobs to do.
         else {
            mttrie->flag = TRIE_BUSY;
            mtplan->active++;
            mtjob_t *job = mttrie->jobs + mttrie->currentjob++;
            pthread_t thread;
            // Start job and detach thread.
            if (pthread_create(&thread, NULL, do_query, job)) {
               alert();
               krash();
            }
            pthread_detach(thread);
            if (verbose) {
               fprintf(stderr, "progress: %.2f%% \r",
                     100*(float)(mtplan->jobsdone)/njobs);
            }
         }
      }

      // If max thread number is reached, wait for a thread.
      while (mtplan->active == thrmax) {
         pthread_cond_wait(mtplan->monitor, mtplan->mutex);
      }

      pthread_mutex_unlock(mtplan->mutex);

   }

   return;

}


void *
do_query
(
   void * args
)  
{
   // Unpack arguments.
   mtjob_t  * job    = (mtjob_t*) args;
   gstack_t * useqS  = job->useqS;
   trie_t   * trie   = job->trie;
   lookup_t * lut    = job->lut;
   const int  tau    = job->tau;
   node_t * node_pos = job->node_pos;

   // Create local hit stack.
   gstack_t **hits = new_tower(tau+1);
   if (hits == NULL) {
      alert();
      krash();
   }

   useq_t * last_query = NULL;

   for (int i = job->start ; i <= job->end ; i++) {
      useq_t *query = (useq_t *) useqS->items[i];
      int do_search = lut_search(lut, query) == 1;

      // Insert the new sequence in the lut and trie, but let
      // the last pointer to NULL so that the query does not
      // find itself upon search.
      void **data = NULL;
      if (job->build) {
         if (lut_insert(lut, query)) {
            alert();
            krash();
         }
         data = insert_string_wo_malloc(trie, query->seq, &node_pos);
         if (data == NULL || *data != NULL) {
            alert();
            krash();
         }
      }

      if (do_search) {
         int trail = 0;
         if (i < job->end) {
            useq_t *next_query = (useq_t *) useqS->items[i+1];
            // The 'while' condition is guaranteed to be false
            // before the end of the 'char' arrays because all
            // the queries have the same length and are different.
            while (query->seq[trail] == next_query->seq[trail]) {
               trail++;
            }
         }

         // Compute start height.
         int start = 0;
         if (last_query != NULL) {
            while(query->seq[start] == last_query->seq[start]) start++;
         }

         // Clear hit stack. //
         for (int j = 0 ; hits[j] != TOWER_TOP ; j++) {
            hits[j]->nitems = 0;
         }

         // Search the trie. //
         int err = search(trie, query->seq, tau, hits, start, trail);
         if (err) {
            alert();
            krash();
         }

         for (int j = 0 ; hits[j] != TOWER_TOP ; j++) {
            if (hits[j]->nitems > hits[j]->nslots) {
               fprintf(stderr, "warning: incomplete search (%s)\n",
                       query->seq);
               break;
            }
         }

         // Link matching pairs for clustering.
         // Skip dist = 0, as this would be self.
         for (int dist = 1 ; dist < tau+1 ; dist++) {
         for (int j = 0 ; j < hits[dist]->nitems ; j++) {

            useq_t *match = (useq_t *) hits[dist]->items[j];
            // Print the pair.
            if (FORMAT == PE_FASTQ) {
               fprintf(stdout, "%s\t%s\t%d\n",
                     query->info, match->info, dist);
            }
            else { 
               // The query sequences are padded. Remove the
               // pad when printing the pairs.
               int P1, P2;
               for (P1 = 0 ; P1 < strlen(query->seq) ; P1++) {
                  if (query->seq[P1] != ' ') break;
               }
               for (P2 = 0 ; P2 < strlen(match->seq) ; P2++) {
                  if (match->seq[P2] != ' ') break;
               }
               fprintf(stdout, "%s\t%s\t%d\n",
                     query->seq+P1, match->seq+P2, dist);
            }

         }
         }

         last_query = query;

      }

      if (job->build) {
         // Finally set the pointer of the inserted tail node.
         *data = query;
      }
   }
   
   destroy_tower(hits);

   // Flag trie, update thread count and signal scheduler.
   // Use the general mutex. (job->mutex[0])
   pthread_mutex_lock(job->mutex);
   *(job->active) -= 1;
   *(job->jobsdone) += 1;
   *(job->trieflag) = TRIE_FREE;
   pthread_cond_signal(job->monitor);
   pthread_mutex_unlock(job->mutex);

   return NULL;

}


mtplan_t *
plan_mt
(
    int       tau,
    int       height,
    int       medianlen,
    int       ntries,
    gstack_t *useqS
)
// SYNOPSIS:                                                              
//   The scheduler makes the key assumption that the number of tries is   
//   an odd number, which allows to distribute the jobs among as in the   
//   example shown below. The rows indicate blocks of query strings and   
//   the columns are distinct tries. An circle (o) indicates a build job, 
//   a cross (x) indicates a query job, and a dot (.) indicates that the  
//   block is not queried in the given trie.                              
//                                                                        
//                            --- Tries ---                               
//                            1  2  3  4  5                               
//                         1  o  .  .  x  x                               
//                         2  x  o  .  .  x                               
//                         3  x  x  o  .  .                               
//                         4  .  x  x  o  .                               
//                         5  .  .  x  x  o                               
//                                                                        
//   This simple schedule ensures that each trie is built from one query  
//   block and that each block is queried against every other exactly one 
//   time (a query of block i in trie j is the same as a query of block j 
//   in trie i).                                                          
{
   // Initialize plan.
   mtplan_t *mtplan = malloc(sizeof(mtplan_t));
   if (mtplan == NULL) {
      alert();
      krash();
   }

   // Initialize mutex.
   pthread_mutex_t *mutex = malloc((ntries + 1) * sizeof(pthread_mutex_t));
   pthread_cond_t *monitor = malloc(sizeof(pthread_cond_t));
   if (mutex == NULL || monitor == NULL) {
      alert();
      krash();
   }
   for (int i = 0; i < ntries + 1; i++) pthread_mutex_init(mutex + i,NULL);
   pthread_cond_init(monitor,NULL);

   // Initialize 'mttries'.
   mttrie_t *mttries = malloc(ntries * sizeof(mttrie_t));
   if (mttries == NULL) {
      alert();
      krash();
   }

   // Boundaries of the query blocks.
   int Q = useqS->nitems / ntries;
   int R = useqS->nitems % ntries;
   int *bounds = malloc((ntries+1) * sizeof(int));
   for (int i = 0 ; i < ntries+1 ; i++) bounds[i] = Q*i + min(i, R);

   // Preallocated tries.
   // Count with maxlen-1
   long *nnodes = malloc(ntries * sizeof(long));
   for (int i = 0; i < ntries; i++) nnodes[i] =
      count_trie_nodes((useq_t **)useqS->items, bounds[i], bounds[i+1]);

   // Create jobs for the tries.
   for (int i = 0 ; i < ntries; i++) {
      // Remember that 'ntries' is odd.
      int njobs = (ntries+1)/2;
      trie_t *local_trie  = new_trie(height);
      node_t *local_nodes = (node_t *) malloc(nnodes[i] * sizeof(node_t));
      mtjob_t *jobs = malloc(njobs * sizeof(mtjob_t));
      if (local_trie == NULL || jobs == NULL) {
         alert();
         krash();
      }

      // Allocate lookup struct.
      // TODO: Try only one lut as well. (It will always return 1 in the query step though).
      lookup_t * local_lut = new_lookup(medianlen, height, tau);
      if (local_lut == NULL) {
         alert();
         krash();
      }

      mttries[i].flag       = TRIE_FREE;
      mttries[i].currentjob = 0;
      mttries[i].njobs      = njobs;
      mttries[i].jobs       = jobs;

      for (int j = 0 ; j < njobs ; j++) {
         // Shift boundaries in a way that every trie is built
         // exactly once and that no redundant jobs are allocated.
         int idx = (i+j) % ntries;
         int only_if_first_job = j == 0;
         // Specifications of j-th job of the local trie.
         jobs[j].start    = bounds[idx];
         jobs[j].end      = bounds[idx+1]-1;
         jobs[j].tau      = tau;
         jobs[j].build    = only_if_first_job;
         jobs[j].useqS    = useqS;
         jobs[j].trie     = local_trie;
         jobs[j].node_pos = local_nodes;
         jobs[j].lut      = local_lut;
         jobs[j].mutex    = mutex;
         jobs[j].monitor  = monitor;
         jobs[j].jobsdone = &(mtplan->jobsdone);
         jobs[j].trieflag = &(mttries[i].flag);
         jobs[j].active   = &(mtplan->active);
         // Mutex ids. (mutex[0] is reserved for general mutex)
         jobs[j].queryid  = idx + 1;
         jobs[j].trieid   = i + 1;
      }
   }

   free(bounds);

   mtplan->active = 0;
   mtplan->ntries = ntries;
   mtplan->jobsdone = 0;
   mtplan->mutex = mutex;
   mtplan->monitor = monitor;
   mtplan->tries = mttries;

   return mtplan;

}

long
count_trie_nodes
(
 useq_t ** seqs,
 int     start,
 int     end
)
{
   int  seqlen = strlen(seqs[start]->seq) - 1;
   long count = seqlen;
   for (int i = start+1; i < end; i++) {
      char * a = seqs[i-1]->seq;
      char * b  = seqs[i]->seq;
      int prefix = 0;
      while (a[prefix] == b[prefix]) prefix++;
      count += seqlen - prefix;
   }
   return count;
}


int
seqsort
(
 useq_t ** data,
 int       numels,
 int       thrmax
)
// SYNOPSIS:                                                              
//   Recursive merge sort for 'useq_t' arrays, tailored for the
//   problem of sorting merging identical sequences. When two
//   identical sequences are detected during the sort, they are
//   merged into a single one with more counts, and one of them
//   is destroyed (freed).
//
// PARAMETERS:                                                            
//   data:       an array of pointers to each element.                    
//   numels:     number of elements, i.e. size of 'data'.                 
//   thrmax: number of threads.                                       
//                                                                        
// RETURN:                                                                
//   Number of unique elements.                               
//                                                                        
// SIDE EFFECTS:                                                          
//   Pointers to repeated elements are set to NULL.
{
   // Copy to buffer.
   useq_t **buffer = malloc(numels * sizeof(useq_t *));
   memcpy(buffer, data, numels * sizeof(useq_t *));

   // Prepare args struct.
   sortargs_t args;
   args.buf0   = data;
   args.buf1   = buffer;
   args.size   = numels;
   // There are two alternating buffers for the merge step.
   // 'args.b' alternates on every call to 'nukesort()' to
   // keep track of which is the source and which is the
   // destination. It has to be initialized to 0 so that
   // sorted elements end in 'data' and not in 'buffer'.
   args.b      = 0;
   args.thread = 0;
   args.repeats = 0;

   // Allocate a number of threads that is a power of 2.
   while ((thrmax >> (args.thread + 1)) > 0) args.thread++;

   nukesort(&args);

   free(buffer);
   return numels - args.repeats;

}

void *
nukesort
(
 void * args
)
// SYNOPSIS:
//   Recursive part of 'seqsort'. The code of 'nukesort()' is
//   dangerous and should not be reused. It uses a very special
//   sort order designed for starcode, it destroys some elements
//   and sets them to NULL as it sorts.
//
// ARGUMENTS:
//   args: a sortargs_t struct (see private header file).
//
// RETURN:
//   NULL pointer, regardless of the input
//
// SIDE EFFECTS:
//   Sorts the array of 'useq_t' specified in 'args'.
{

   sortargs_t * sortargs = (sortargs_t *) args;
   if (sortargs->size < 2) return NULL;

   // Next level params.
   sortargs_t arg1 = *sortargs, arg2 = *sortargs;
   arg1.size /= 2;
   arg2.size = arg1.size + arg2.size % 2;
   arg2.buf0 += arg1.size;
   arg2.buf1 += arg1.size;
   arg1.b = arg2.b = (arg1.b + 1) % 2;

   // Either run threads or DIY.
   if (arg1.thread) {
      // Decrease one level.
      arg1.thread = arg2.thread = arg1.thread - 1;
      // Create threads.
      pthread_t thread1, thread2;
      if ( pthread_create(&thread1, NULL, nukesort, &arg1) ||
           pthread_create(&thread2, NULL, nukesort, &arg2) ) {
         alert();
         krash();
      }
      // Wait for threads.
      pthread_join(thread1, NULL);
      pthread_join(thread2, NULL);
   }
   else {
      nukesort(&arg1);
      nukesort(&arg2);
   }

   // Separate data and buffer (b specifies which is buffer).
   useq_t ** l = (sortargs->b ? arg1.buf0 : arg1.buf1);
   useq_t ** r = (sortargs->b ? arg2.buf0 : arg2.buf1);
   useq_t ** buf = (sortargs->b ? arg1.buf1 : arg1.buf0);

   int i = 0;
   int j = 0;
   int idx = 0;
   int cmp = 0;
   int repeats = 0;

   // Merge sets
   while (i+j < sortargs->size) {
      // Only NULLS at the end of the buffers.
      if (j == arg2.size || r[j] == NULL) {
         // Right buffer is exhausted. Copy left buffer...
         memcpy(buf+idx, l+i, (arg1.size-i) * sizeof(useq_t *));
         break;
      }
      if (i == arg1.size || l[i] == NULL) {
         // ... or vice versa.
         memcpy(buf+idx, r+j, (arg2.size-j) * sizeof(useq_t *));
         break;
      }
      if (l[i] == NULL && r[j] == NULL) break;

      // Do the comparison.
      useq_t *ul = (useq_t *) l[i];
      useq_t *ur = (useq_t *) r[j];
      int sl = strlen(ul->seq);
      int sr = strlen(ur->seq);
      if (sl == sr) cmp = strcmp(ul->seq, ur->seq);
      else cmp = sl < sr ? -1 : 1;

      if (cmp == 0) {
         // Identical sequences, this is the nuke part.
         ul->count += ur->count;
         destroy_useq(ur);
         buf[idx++] = l[i++];
         j++;
         repeats++;
      } 
      else if (cmp < 0) buf[idx++] = l[i++];
      else              buf[idx++] = r[j++];

   }

   // Accumulate repeats
   sortargs->repeats = repeats + arg1.repeats + arg2.repeats;

   // Pad with NULLS.
   int offset = sortargs->size - sortargs->repeats;
   memset(buf+offset, 0, sortargs->repeats*sizeof(useq_t *));
   
   return NULL;

}


gstack_t *
read_rawseq
(
   FILE     * inputf,
   gstack_t * uSQ
)
{

   ssize_t nread;
   size_t nchar = M;
   char copy[MAXBRCDLEN];
   char *line = malloc(M * sizeof(char));
   if (line == NULL) {
      alert();
      krash();
   }

   char *seq = NULL;
   int count = 0;
   int lineno = 0;

   while ((nread = getline(&line, &nchar, inputf)) != -1) {
      lineno++;
      if (line[nread-1] == '\n') line[nread-1] = '\0';
      if (sscanf(line, "%s\t%d", copy, &count) != 2) {
         count = 1;
         seq = line;
      }
      else {
         seq = copy;
      }
      size_t seqlen = strlen(seq);
      if (seqlen > MAXBRCDLEN) {
         fprintf(stderr, "max sequence length exceeded (%d)\n",
               MAXBRCDLEN);
         fprintf(stderr, "offending sequence:\n%s\n", seq);
         abort();
      }
      for (size_t i = 0 ; i < seqlen ; i++) {
         if (!valid_DNA_char[(int)seq[i]]) {
            fprintf(stderr, "invalid input\n");
            fprintf(stderr, "offending sequence:\n%s\n", seq);
            abort();
         }
      }
      useq_t *new = new_useq(count, seq, NULL);
      push(new, &uSQ);
   }

   free(line);
   return uSQ;

}


gstack_t *
read_fasta
(
   FILE     * inputf,
   gstack_t * uSQ
)
{

   ssize_t nread;
   size_t nchar = M;
   char *line = malloc(M * sizeof(char));
   if (line == NULL) {
      alert();
      krash();
   }

   int lineno = 0;

   while ((nread = getline(&line, &nchar, inputf)) != -1) {
      lineno++;
      // Strip newline character.
      if (line[nread-1] == '\n') line[nread-1] = '\0';

      if (lineno %2 == 0) {
         size_t seqlen = strlen(line);
         if (seqlen > MAXBRCDLEN) {
            fprintf(stderr, "max sequence length exceeded (%d)\n",
                  MAXBRCDLEN);
            fprintf(stderr, "offending sequence:\n%s\n", line);
            abort();
         }
         for (size_t i = 0 ; i < seqlen ; i++) {
            if (!valid_DNA_char[(int)line[i]]) {
               fprintf(stderr, "invalid input\n");
               fprintf(stderr, "offending sequence:\n%s\n", line);
               abort();
            }
         }
         useq_t *new = new_useq(1, line, NULL);
         if (new == NULL) {
            alert();
            krash();
         }
         push(new, &uSQ);
      }
   }

   free(line);
   return uSQ;

}


gstack_t *
read_fastq
(
   FILE     * inputf,
   gstack_t * uSQ
)
{

   ssize_t nread;
   size_t nchar = M;
   char *line = malloc(M * sizeof(char));
   if (line == NULL) {
      alert();
      krash();
   }

   char seq[M] = {0};
   int lineno = 0;

   while ((nread = getline(&line, &nchar, inputf)) != -1) {
      lineno++;
      // Strip newline character.
      if (line[nread-1] == '\n') line[nread-1] = '\0';
      if (lineno % 4 == 2) {
         size_t seqlen = strlen(line);
         if (seqlen > MAXBRCDLEN) {
            fprintf(stderr, "max sequence length exceeded (%d)\n",
                  MAXBRCDLEN);
            fprintf(stderr, "offending sequence:\n%s\n", line);
            abort();
         }
         for (size_t i = 0 ; i < seqlen ; i++) {
            if (!valid_DNA_char[(int)line[i]]) {
               fprintf(stderr, "invalid input\n");
               fprintf(stderr, "offending sequence:\n%s\n", line);
               abort();
            }
         }
         strncpy(seq, line, M);
      }
      else if (lineno % 4 == 0) {
         useq_t *new = new_useq(1, seq, NULL);
         if (new == NULL) {
            alert();
            krash();
         }
         push(new, &uSQ);
      }
   }

   free(line);
   return uSQ;

}


gstack_t *
read_PE_fastq
(
   FILE     * inputf1,
   FILE     * inputf2,
   gstack_t * uSQ
)
{

   char c1 = fgetc(inputf1);
   char c2 = fgetc(inputf2);
   if (c1 != '@' || c2 != '@') {
      fprintf(stderr, "input not a pair of fastq files\n");
      abort();
   }
   if (ungetc(c1, inputf1) == EOF || ungetc(c2, inputf2) == EOF) {
      alert();
      krash();
   }

   ssize_t nread;
   size_t nchar = M;
   char *line1 = malloc(M * sizeof(char));
   char *line2 = malloc(M * sizeof(char));
   if (line1 == NULL && line2 == NULL) {
      alert();
      krash();
   }

   char seq1[M] = {0};
   char seq2[M] = {0};
   char seq[2*M+8] = {0};
   char info[4*M] = {0};
   int lineno = 0;

   char sep[STARCODE_MAX_TAU+2] = {0};
   memset(sep, '-', STARCODE_MAX_TAU+1);
   while ((nread = getline(&line1, &nchar, inputf1)) != -1) {
      lineno++;
      // Strip newline character.
      if (line1[nread-1] == '\n') line1[nread-1] = '\0';

      // Read line from second file and strip newline.
      if ((nread = getline(&line2, &nchar, inputf2)) == -1) {
         fprintf(stderr, "non conformable paired-end fastq files\n");
         abort();
      }
      if (line2[nread-1] == '\n') line2[nread-1] = '\0';

      if (lineno % 4 == 2) {
         size_t seqlen1 = strlen(line1);
         size_t seqlen2 = strlen(line2);
         if (seqlen1 > MAXBRCDLEN || seqlen2 > MAXBRCDLEN) {
            fprintf(stderr, "max sequence length exceeded (%d)\n",
                  MAXBRCDLEN);
            fprintf(stderr, "offending sequences:\n%s\n%s\n",
                  line1, line2);
            abort();
         }
         for (size_t i = 0 ; i < seqlen1 ; i++) {
            if (!valid_DNA_char[(int)line1[i]]) {
               fprintf(stderr, "invalid input\n");
               fprintf(stderr, "offending sequence:\n%s\n", line1);
               abort();
            }
         }
         for (size_t i = 0 ; i < seqlen2 ; i++) {
            if (!valid_DNA_char[(int)line2[i]]) {
               fprintf(stderr, "invalid input\n");
               fprintf(stderr, "offending sequence:\n%s\n", line2);
               abort();
            }
         }
         strncpy(seq1, line1, M);
         strncpy(seq2, line2, M);
      }
      else if (lineno % 4 == 0) {
         // No need for the headers, the 'info' member is
         // used to hold a string representation of the pair.
         int scheck;
         scheck = snprintf(info, 2*M, "%s/%s", seq1, seq2);
         if (scheck < 0 || scheck > 2*M-1) {
             alert();
             krash();
         }
         scheck = snprintf(seq, 2*M+8, "%s%s%s", seq1, sep, seq2);
         if (scheck < 0 || scheck > 2*M+7) {
            alert();
            krash();
         }
         useq_t *new = new_useq(1, seq, info);
         if (new == NULL) {
            alert();
            krash();
         }
         push(new, &uSQ);
      }
   }

   free(line1);
   free(line2);
   return uSQ;

}


gstack_t *
read_file
(
   FILE      * inputf1,
   FILE      * inputf2,
   const int   verbose
)
{

   if (inputf2 != NULL) FORMAT = PE_FASTQ;
   else {
      // Read first line of the file to guess format.
      // Store in global variable FORMAT.
      char c = fgetc(inputf1);
      switch(c) {
         case EOF:
            // Empty file.
            return NULL;
         case '>':
            FORMAT = FASTA;
            if (verbose) fprintf(stderr, "FASTA format detected\n");
            break;
         case '@':
            FORMAT = FASTQ;
            if (verbose) fprintf(stderr, "FASTQ format detected\n");
            break;
         default:
            FORMAT = RAW;
            if (verbose) fprintf(stderr, "raw format detected\n");
      }

      if (ungetc(c, inputf1) == EOF) {
         alert();
         krash();
      }
   }

   gstack_t *uSQ = new_gstack();
   if (uSQ == NULL) {
      alert();
      krash();
   }

   if (FORMAT == RAW)      return read_rawseq(inputf1, uSQ);
   if (FORMAT == FASTA)    return read_fasta(inputf1, uSQ);
   if (FORMAT == FASTQ)    return read_fastq(inputf1, uSQ);
   if (FORMAT == PE_FASTQ) return read_PE_fastq(inputf1, inputf2, uSQ);

   return NULL;

}


int
pad_useq
(
   gstack_t * useqS,
   int      * median
)
{

   // Compute maximum length.
   int maxlen = 0;
   for (int i = 0 ; i < useqS->nitems ; i++) {
      useq_t *u = useqS->items[i];
      int len = strlen(u->seq);
      if (len > maxlen) maxlen = len;
   }

   // Alloc median bins. (Initializes to 0)
   int  * count = calloc((maxlen + 1), sizeof(int));
   char * spaces = malloc((maxlen + 1) * sizeof(char));
   if (spaces == NULL || count == NULL) {
      alert();
      krash();
   }
   for (int i = 0 ; i < maxlen ; i++) spaces[i] = ' ';
   spaces[maxlen] = '\0';

   // Pad all sequences with spaces.
   for (int i = 0 ; i < useqS->nitems ; i++) {
      useq_t *u = useqS->items[i];
      int len = strlen(u->seq);
      count[len]++;
      if (len == maxlen) continue;
      // Create a new sequence with padding characters.
      char *padded = malloc((maxlen + 1) * sizeof(char));
      if (padded == NULL) {
         alert();
         krash();
      }
      memcpy(padded, spaces, maxlen + 1);
      memcpy(padded+maxlen-len, u->seq, len);
      free(u->seq);
      u->seq = padded;
   }

   // Compute median.
   *median = 0;
   int ccount = 0;
   do {
      ccount += count[++(*median)];
   } while (ccount < useqS->nitems / 2);

   // Free and return.
   free(count);
   free(spaces);
   return maxlen;

}


void
unpad_useq
(
   gstack_t *useqS
)
{
   // Take the length of the first sequence (assume all
   // sequences have the same length).
   int len = strlen(((useq_t *) useqS->items[0])->seq);
   for (int i = 0 ; i < useqS->nitems ; i++) {
      useq_t *u = (useq_t *) useqS->items[i];
      int pad = 0;
      while (u->seq[pad] == ' ') pad++;
      // Create a new sequence without paddings characters.
      char *unpadded = malloc((len - pad + 1) * sizeof(char));
      if (unpadded == NULL) {
         alert();
         krash();
      }
      memcpy(unpadded, u->seq + pad, len - pad + 1);
      free(u->seq);
      u->seq = unpadded;
   }
   return;
}


lookup_t *
new_lookup
(
 int slen,
 int maxlen,
 int tau
)
{

   lookup_t * lut = (lookup_t *) malloc(2*sizeof(int) + sizeof(int *) +
         (tau+1)*sizeof(char *));
   if (lut == NULL) {
      alert();
      return NULL;
   }

   // Target size.
   int k   = slen / (tau + 1);
   int rem = tau - slen % (tau + 1);

   // Set parameters.
   lut->slen  = maxlen;
   lut->kmers = tau + 1;
   lut->klen  = (int *) malloc(lut->kmers * sizeof(int));
   
   // Compute k-mer lengths.
   if (k > MAX_K_FOR_LOOKUP)
      for (int i = 0; i < tau + 1; i++) lut->klen[i] = MAX_K_FOR_LOOKUP;
   else
      for (int i = 0; i < tau + 1; i++) lut->klen[i] = k - (rem-- > 0);

   // Allocate lookup tables.
   for (int i = 0; i < tau + 1; i++) {
      lut->lut[i] = calloc(1 << max(0,(2*lut->klen[i] - 3)),
            sizeof(char));
      if (lut->lut[i] == NULL) {
         while (--i >= 0) {
            free(lut->lut[i]);
         }  
         free(lut);
         alert();
         return NULL;
      }
   }

   return lut;

}


void
destroy_lookup
(
   lookup_t * lut
)
{
   for (int i = 0 ; i < lut->kmers ; i++) free(lut->lut[i]);
   free(lut->klen);
   free(lut);
}


int
lut_search
(
 lookup_t * lut,
 useq_t   * query
)
// SYNOPSIS:
//   Perform of a lookup search of the query and determine whether
//   at least one of the k-mers extracted from the query was inserted
//   in the lookup table. If this is not the case, the trie search can
//   be skipped because the query cannot have a match for the given
//   tau.
//   
// ARGUMENTS:
//   lut: the lookup table to search
//   query: the query as a useq.
//
// RETURN:
//   1 if any of the k-mers extracted from the query is in the
//   lookup table, 0 if not, and -1 in case of failure.
//
// SIDE-EFFECTS:
//   None.
{
   // Start from the end of the sequence. This will avoid potential
   // misalignments on the first kmer due to insertions.
   int offset = lut->slen;
   // Iterate for all k-mers and for ins/dels.
   for (int i = lut->kmers - 1; i >= 0; i--) {
      offset -= lut->klen[i];
      for (int j = -(lut->kmers - 1 - i); j <= lut->kmers - 1 - i; j++) {
         // If sequence contains 'N' seq2id will return -1.
         int seqid = seq2id(query->seq + offset + j, lut->klen[i]);
         // Make sure to never proceed passed the end of string.
         if (seqid == -2) return -1;
         if (seqid == -1) continue;
         // The lookup table proper is implemented as a bitmap.
         if ((lut->lut[i][seqid/8] >> (seqid%8)) & 1) return 1;
      }
   }

   return 0;

}


int
lut_insert
(
 lookup_t * lut,
 useq_t   * query
)
{

   int offset = lut->slen;
   for (int i = lut->kmers-1; i >= 0; i--) {
      offset -= lut->klen[i];
      int seqid = seq2id(query->seq + offset, lut->klen[i]);
      // The lookup table proper is implemented as a bitmap.
      if (seqid >= 0) lut->lut[i][seqid/8] |= (1 << (seqid%8));
      // Make sure to never proceed passed the end of string.
      else if (seqid == -2) return 1;
   }

   return 0;
}


int
seq2id
(
  char * seq,
  int    slen
)
{
   int seqid = 0;
   for (int i = 0; i < slen; i++) {
      // Padding spaces are substituted by 'A'. It does not hurt
      // anyway to generate some false positives.
      if (seq[i] == 'A' || seq[i] == 'a' || seq[i] == ' ') { }
      else if (seq[i] == 'C' || seq[i] == 'c') seqid += 1;
      else if (seq[i] == 'G' || seq[i] == 'g') seqid += 2;
      else if (seq[i] == 'T' || seq[i] == 't') seqid += 3;
      else return seq[i] == 0 ? -2 : -1;
      if (i < slen - 1) seqid <<= 2;
   }

   return seqid;

}


useq_t *
new_useq
(
   int    count,
   char * seq,
   char * info
)
{

   // Check input.
   if (seq == NULL) return NULL;

   useq_t *new = calloc(1, sizeof(useq_t));
   if (new == NULL) {
      alert();
      krash();
   }
   new->seq = strdup(seq);
   new->count = count;
   if (info != NULL) {
      new->info = strdup(info);
      if (new->info == NULL) {
         alert();
         krash();
      }
   }

   return new;

}


void
destroy_useq
(
   useq_t *useq
)
{
   if (useq->matches != NULL) destroy_tower(useq->matches);
   if (useq->info != NULL) free(useq->info);
   free(useq->seq);
   free(useq);
}


void
krash
(void)
{
   fprintf(stderr,
      "starcode has crashed, please contact guillaume.filion@gmail.com "
      "for support with this issue.\n");
   abort();
}
