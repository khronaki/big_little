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