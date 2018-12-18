/*************************************************************************************/
/*      Copyright 2014 Barcelona Supercomputing Center                               */
/*                                                                                   */
/*      This file is part of the NANOS++ library.                                    */
/*                                                                                   */
/*      NANOS++ is free software: you can redistribute it and/or modify              */
/*      it under the terms of the GNU Lesser General Public License as published by  */
/*      the Free Software Foundation, either version 3 of the License, or            */
/*      (at your option) any later version.                                          */
/*                                                                                   */
/*      NANOS++ is distributed in the hope that it will be useful,                   */
/*      but WITHOUT ANY WARRANTY; without even the implied warranty of               */
/*      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                */
/*      GNU Lesser General Public License for more details.                          */
/*                                                                                   */
/*      You should have received a copy of the GNU Lesser General Public License     */
/*      along with NANOS++.  If not, see <http://www.gnu.org/licenses/>.             */
/*************************************************************************************/
#include "dasthread.hpp"
#include "dataaccess_decl.hpp"
#include "system_decl.hpp"
#include "workdescriptor_decl.hpp"
#include <iostream>
#include <queue>
#include "atomic.hpp"
#include "debug.hpp"

using namespace nanos;
using namespace nanos::ext;
//void dispatchWork( DASTWork* work, DASTData* dastData, BaseThread *worker );
WD* DASThread::submitingWD = NULL;

DASTData::DASTData () : _upperLimit( DASThread::INIT_UPPER_LIMIT ), _lowerLimit( DASThread::INIT_LOWER_LIMIT )
{
   _flags.wait_lower = false;
   _flags.paused_last = false;
}

void DASTData::pushNewWork( WD *creator, WD *work, size_t numDeps, DataAccess* deps )
{
   NANOS_INSTRUMENT ( static nanos_event_key_t key = sys.getInstrumentation()->getInstrumentationDictionary()->getEventKey("dast-operation") );
   NANOS_INSTRUMENT ( nanos_event_value_t val = sys.getInstrumentation()->getInstrumentationDictionary()->getEventValue("dast-operation","NANOS_DAST_SUBMIT_NEW") );
   NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseOpenStateAndBurst ( NANOS_CREATION, key, val ) );

//   work->setInDAST( true );

   DASTWork* element = new DASTWork;
   element->_creator = creator;
   element->_work = work;
   element->_numDependencies = numDeps;
   element->_dependencies = ( DataAccess * )( malloc( sizeof( DataAccess )*numDeps ) );
   memcpy( element->_dependencies, deps, sizeof( DataAccess )*numDeps );
   element->_type = DAST_WORK_IN;
   {
      LockBlock lock( _qLock );
      _queue.push( element );
      memoryFence();
   }

   NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseCloseStateAndBurst ( key, 0 ) );
}

bool DASTData::tryPushNewWork( WD *creator, WD *work, size_t numDeps, DataAccess* deps )
{
   if ( sys.isDastForced() || ( creator == DASThread::submitingWD ) || ( __sync_bool_compare_and_swap( &( DASThread::submitingWD ), NULL, creator ) ) ) {
      pushNewWork( creator, work, numDeps, deps );
      return true;
   }
   return false;
}

void DASTData::pushDoneWork( WD *work, bool schedule )
{
   NANOS_INSTRUMENT ( static nanos_event_key_t key = sys.getInstrumentation()->getInstrumentationDictionary()->getEventKey("dast-operation") );
   NANOS_INSTRUMENT ( nanos_event_value_t val = sys.getInstrumentation()->getInstrumentationDictionary()->getEventValue("dast-operation","NANOS_DAST_SUBMIT_DONE") );
   NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseOpenStateAndBurst ( NANOS_CREATION, key, val ) );

   work->setInDAST( true );

   DASTWork* element = new DASTWork;
   element->_work = work;
   element->_numDependencies = schedule ? 1 : 0;
   element->_type = DAST_WORK_OUT;
   {
      LockBlock lock( _qLock );
      _queue.push( element );
      memoryFence();
   }

   NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseCloseStateAndBurst ( key, 0 ) );
}

bool DASTData::tryPushDoneWork( WD *work, bool schedule )
{
   if ( DASThread::submitingWD == work ) {
      DASThread::submitingWD = NULL;
   }
   if ( sys.isDastForced() || ( shouldPushWork() && sys.getSchedulerStats().getReadyTasks() >= DASThread::READY_TASKS_THRESHOLD ) ) {
      pushDoneWork( work, schedule );
      return true;
   }
   return false;
}

void DASTData::pushDeleteWork( WD *work )
{
   NANOS_INSTRUMENT ( static nanos_event_key_t key = sys.getInstrumentation()->getInstrumentationDictionary()->getEventKey("dast-operation") );
   NANOS_INSTRUMENT ( nanos_event_value_t val = sys.getInstrumentation()->getInstrumentationDictionary()->getEventValue("dast-operation","NANOS_DAST_SUBMIT_DEL") );
   NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseOpenStateAndBurst ( NANOS_RUNTIME, key, val ) );

//   work->setInDAST( true );

   DASTWork* element = new DASTWork;
   element->_work = work;
   element->_type = DAST_WORK_DELETE;
   {
      LockBlock lock( _qLock );
      _queue.push( element );
      memoryFence();
   }

   NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseCloseStateAndBurst ( key, 0 ) );
}

bool DASTData::tryPop ( DASTWork* &result )
{
   bool found = false;
   if ( _queue.empty() ) return false;

   memoryFence();
   {
      LockBlock lock( _qLock );

      if ( !_queue.empty() ) {
         result = _queue.front();
         _queue.pop();
         found = true;
      }
   }

   return found;
}

bool DASTData::hasRequests()
{
   memoryFence();
   return !_queue.empty();
}

bool DASTData::shouldPushWork ()
{
   bool ret = true;
   if ( _flags.wait_lower ) {
      ret = _queue.size() < _lowerLimit;
      if ( _flags.paused_last && ret ) {
         _lowerLimit *= 2;
         _upperLimit *= 2;
      }
      _flags.wait_lower = !ret;
      _flags.paused_last = false;
   } else if ( _queue.size() > _upperLimit ) {
      ret = false;
      _flags.wait_lower = true;
      _flags.paused_last = true;
   }
   return ret;
}

void DASTData::dispatchWork( DASTWork* work, DASTData* dastData, BaseThread *worker ) 
{
   if ( work->_type == DAST_WORK_IN ) {
               NANOS_INSTRUMENT ( static nanos_event_key_t key = sys.getInstrumentation()->getInstrumentationDictionary()->getEventKey("dast-operation") );
               NANOS_INSTRUMENT ( nanos_event_value_t val = sys.getInstrumentation()->getInstrumentationDictionary()->getEventValue("dast-operation","NANOS_DAST_CREATION"); );
               NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseOpenStateAndBurst ( NANOS_DAST_CREATION, key, val ) );

               SchedulePolicy* policy = sys.getDefaultSchedulePolicy();
               policy->onSystemSubmit( *( work->_work ), SchedulePolicy::DAST_SUBMIT );
               ( work->_creator )->submitWithDependencies( *( work->_work ), work->_numDependencies, work->_dependencies );
//               work->_work->setInDAST( false );
               free( ( void * )work->_dependencies );

               if ( DASThread::submitingWD == work->_creator && !dastData->hasRequests() ) {
                  DASThread::submitingWD = NULL;
               }

               NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseCloseStateAndBurst ( key, 0 ) );
   } else if ( work->_type == DAST_WORK_OUT ) {
               NANOS_INSTRUMENT ( static nanos_event_key_t key = sys.getInstrumentation()->getInstrumentationDictionary()->getEventKey("dast-operation") );
               NANOS_INSTRUMENT ( nanos_event_value_t val = sys.getInstrumentation()->getInstrumentationDictionary()->getEventValue("dast-operation","NANOS_DAST_DONE") );
               NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseOpenStateAndBurst ( NANOS_DAST_DONE, key, val ) );

               worker->addNextWD( worker->getTeam()->getSchedulePolicy().atBeforeExit( worker, *( work->_work ), work->_numDependencies ) );
               ( work->_work )->dependenciesDone();
               ( work->_work )->notifyParent();
               ( work->_work )->setInDAST( false );

               NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseCloseStateAndBurst ( key, 0 ) );
   } else if ( work->_type == DAST_WORK_DELETE ) {
               NANOS_INSTRUMENT ( static nanos_event_key_t key = sys.getInstrumentation()->getInstrumentationDictionary()->getEventKey("dast-operation") );
               NANOS_INSTRUMENT ( nanos_event_value_t val = sys.getInstrumentation()->getInstrumentationDictionary()->getEventValue("dast-operation","NANOS_DAST_DEL"); );
               NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseOpenStateAndBurst ( NANOS_DAST_DEL, key, val ) );

               ( work->_work )->~WorkDescriptor();
               delete[] (char *)work->_work;

               NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseCloseStateAndBurst ( key, 0 ) );
   } else {
               fatal( "Invalid DASTWork type" );
            }
         

}

#define BIG 0 
#define LITTLE 1
DASTData *_myRequests = NULL;
Lock _reqLock;

void DASThread::workLoop ()
{
   //int freq = 0;
   //int DASTFreq = sys.getDASTFreq();
   int DASTtype = BIG;
   int currType = ( ( myThread->runningOn()->getId() >= sys.getHPFrom() && myThread->runningOn()->getId() <= sys.getHPTo() ) ? BIG : LITTLE );
   fprintf(stderr, "I am a DAST thread. My id is: %d NUMWORKERS = %d\n", myThread->runningOn()->getId(), sys.getNumWorkers() );
   BaseThread* thread = myThread;
   {
      LockBlock lock( _reqLock );
      if(_myRequests == NULL) {
         _myRequests = new DASTData();
      }
   }
   //if(currType != DASTtype) return;
   while ( myThread->isRunning() ) { //&& currType == DASTtype) {
      if(_myRequests->hasRequests()) fprintf(stderr, "There are requests in myRequests!!\n");
      memoryFence();
	  if(currType == DASTtype) {
         //fprintf(stderr, "I am the --- first --- DAST core executing tasks!\n");
         //memoryFence();
         if ( !thread->isRunning() ) {
            fprintf(stderr, "BIG DAST must stop!! 1\n");
            break;
         }
         if (  sys.mustDASTStop()  ) {
            fprintf(stderr, "BIG DAST must stop!! 2\n");
            break;
         }
         //First check if there are requests pushed by the other DAST
         unsigned opsCount = 0;
         DASTWork* work;
         while ( opsCount++ < MAX_OPS_THREAD ) {
            if(  _myRequests->tryPop( work ) ) {
//               fprintf(stderr, "THREAD %d: Dispatching work in myRequests...\n", myThread->runningOn()->getId());
               _myRequests->dispatchWork( work, _myRequests, sys.getWorker(0) );
               delete work;
            }
         }

         for ( int workerIdx = 0; workerIdx <= sys.getNumWorkers(); ++workerIdx ) {
            BaseThread *worker = ( workerIdx == sys.getNumWorkers() ? thread : sys.getWorker( workerIdx ) );

            if ( worker->getTeamData() != thread->getTeamData() ) {
               thread->enterTeam( worker->getTeamData() );
            }
            opsCount = 0;
            DASTData* dastData = worker->getDASTData();
            work = NULL;
            while ( opsCount++ < MAX_OPS_THREAD && dastData->tryPop( work ) ) {
//               fprintf(stderr, "THREAD %d: Dispatching work from main queue of worker that runs on %d\n", myThread->runningOn()->getId(), worker->runningOn()->getId());
			   dastData->dispatchWork( work, dastData, worker );
               delete work;
            }
         }
/*	     if ( freq >= DASTFreq && sys.getSchedulerStats().getReadyTasks() > 0 ) {
            if ( sys.getSchedulerConf().getSchedulerEnabled() && thread->getTeam() != NULL ) {
               // The thread is not paused, mark it as so
               thread->unpause();
               WD* next =  thread->getTeam()->getSchedulePolicy().atIdle ( thread );
			   //switchWD
			   if(next) {
                  if ( next->started() ){
	                 Scheduler::switchTo(next);
                  }
	              else {
	                 if ( Scheduler::inlineWork ( next, false ) ) {
	                    if ( next->isInDAST() ) {
	                       //thread->getDASTData()->pushDeleteWork( next );
	                       _myRequests->pushDeleteWork( next );
	                    } else {
	                       next->~WorkDescriptor();
	                       delete[] (char *)next;
	                    }
	                 }
			      }
			      freq = 0;
			     // fprintf(stderr, "EXECUTION of WD DONE!! by the first DAST thread\n"); 
			   }
			   //else fprintf(stderr, "WD is NULL!\n");
		    }
	     }
	     freq++;
  */    } //if currType == DASTtype
      else { //2nd DAST thread (LITTLE)
         if ( !thread->isRunning() ) {
            fprintf(stderr, "Little DAST must stop!! 1\n");
            break;
         }
         if (  sys.mustDASTStop()  ) {
            fprintf(stderr, "Little DAST must stop!! 2\n");
            break;
         }
         BaseThread *worker = ( 0 == sys.getNumWorkers() ? thread : sys.getWorker( 0 ) );

         if ( worker->getTeamData() != thread->getTeamData() ) {
           fprintf(stderr, "Little DAST is entering the thread team...\n");
           thread->enterTeam( worker->getTeamData() );
         }

	     if ( freq >= DASTFreq && sys.getSchedulerStats().getReadyTasks() > 0 ) {
            if ( sys.getSchedulerConf().getSchedulerEnabled() && thread->getTeam() != NULL ) {
               // The thread is not paused, mark it as so
               thread->unpause();
               WD* next =  thread->getTeam()->getSchedulePolicy().atIdle ( thread );
			   //switchWD
			   if(next) {
                  if ( next->started() ){
                     fprintf(stderr, "Switching to %p WD \n", next);
                     //next->setInDAST(true);
	                 Scheduler::switchTo(next);
                     fprintf(stderr, "EXECUTION of WD  DONE!! by the second DAST thread\n");
                  }
	              else {
                     fprintf(stderr, "THREAD %d: going to inline work... for %p WD\n", myThread->runningOn()->getId(), next);
	                 if ( Scheduler::inlineWork ( next, false ) ) {
	                    if ( next->isInDAST() ) {
                    //       fprintf(stderr, "next %p is in DAST... pushing deleted work in myRequests\n", next);
                           //next->setInDAST(true); //Kallia: try to see if this helps...
                           _myRequests->pushDeleteWork(next);
	                       //thread->getDASTData()->pushDeleteWork( next );
	                    } else {
                           fprintf(stderr, "THREAD %d: next %p is not in DAST... deleting work\n", myThread->runningOn()->getId(), next);
	                       next->~WorkDescriptor();
	                       delete[] (char *)next;
	                    }
	                //  fprintf(stderr, "work INLINED!\n");
                     }
                     fprintf(stderr, "THREAD %d: EXECUTION of WD  DONE!! by the second DAST thread\n", myThread->runningOn()->getId()); 
			      }
                  freq = 0;
			   }
			   //else fprintf(stderr, "WD is NULL!\n");
		    }
	     }


//         fprintf(stderr, "I am the second DAST core executing tasks!\n");
         //memoryFence();
/*         if ( !thread->isRunning() ) {
            fprintf(stderr, "Little DAST must stop!! 1\n");
            break;
         }
         if (  sys.mustDASTStop()  ) {
            fprintf(stderr, "Little DAST must stop!! 2\n");
            break;
         }
         BaseThread *worker = ( 0 == sys.getNumWorkers() ? thread : sys.getWorker( 0 ) );

         if ( worker->getTeamData() != thread->getTeamData() ) {
           fprintf(stderr, "Little DAST is entering the thread team...\n");
           thread->enterTeam( worker->getTeamData() );
         }
	     if ( sys.getSchedulerStats().getReadyTasks() > 0 ) {
            fprintf(stderr, "Get ready tasks from scheduler returned more than 0! Value is: %d\n", (int)sys.getSchedulerStats().getReadyTasks());
            if ( sys.getSchedulerConf().getSchedulerEnabled() && thread->getTeam() != NULL ) {
            // The thread is not paused, mark it as so
               thread->unpause();
               WD* next =  thread->getTeam()->getSchedulePolicy().atIdle ( thread );
               fprintf(stderr, "atIdle returned...\n");
			   //switchWD
			   if(next) {
			   	  //fprintf(stderr, "DAST is going to execute something, WD: %p\n", next);
                  if ( next->started() ){
				       fprintf(stderr, "next WD has started switching to it...\n");
	                 //Scheduler::switchTo(next);
	                 thread->getTeam()->getSchedulePolicy().queue(thread, *next);
				     fprintf(stderr, "Next WD done\n");
                  } 
	              else {
                     fprintf(stderr, "WD is not started!!\n");
	                 if ( Scheduler::inlineWork ( next, false ) ) {
	                    //if ( next->isInDAST() ) {
	                       thread->getDASTData()->pushDeleteWork( next );
	                    //} else {
	                    //   next->~WorkDescriptor();
	                    //   delete[] (char *)next;
	                    //}
	                 }
			      }
			      fprintf(stderr, "EXECUTION of WD DONE!!\n"); 
			      //continue; 
			   }
			   else fprintf(stderr, "WD is NULL!\n");
		    }
	        else {
               if(!sys.getSchedulerConf().getSchedulerEnabled())
                  fprintf(stderr, "scheduler is NOT enabled!\n");
               else
                  fprintf(stderr, "thread team is NULL!\n");
            }
	     }
	     //else fprintf(stderr, "no hay ready tasks!\n");
*/
         ;
      } //else (if currType!= DASTType)
   } //while thread is running
   fprintf(stderr, "DAST Thread number %d is exiting the team...\n", myThread->runningOn()->getId());
   getMyThreadSafe()->exitTeam();
}
