void System::start ()
{
   if ( !_useCaches ) _cachePolicy = System::NONE;

   //! Load hwloc first, in order to make it available for modules
   if ( isHwlocAvailable() )
      loadHwloc();

   // loadNUMAInfo needs _targetThreads when hwloc is not available.
   // Note that it is not its final value!
   _targetThreads = _numThreads;

   // Load & check NUMA config
   loadNUMAInfo();

   // Modules can be loaded now
   loadModules();

   // Increase targetThreads, ask the architecture plugins
   for ( ArchitecturePlugins::const_iterator it = _archs.begin();
        it != _archs.end(); ++it )
   {
      _targetThreads += (*it)->getNumThreads();
   }

   // Instrumentation startup
   NANOS_INSTRUMENT ( sys.getInstrumentation()->filterEvents( _instrumentDefault, _enableEvents, _disableEvents ) );
   NANOS_INSTRUMENT ( sys.getInstrumentation()->initialize() );

   verbose0 ( "Starting runtime" );

   _pmInterface->start();

   int numPes = getNumPEs();
   int masterPeId = 0;
   PE *pe = NULL;

   _pes.reserve ( numPes );
   if ( _enableDAST ) {
      verbose0( "DAST enabled on this execution" );

      pe = createPE ( "smp", getBindingId( 0 ), 0 );
      pe->setNUMANode( getNodeOfPE( pe->getId() ) );
      _pes.push_back ( pe );
      CPU_SET( getBindingId( 0 ), &_cpuActiveSet );

      _dasThread = &pe->startDASThread();
      fatal_cond0( _dasThread == NULL, "PE::startDASThread retuned NULL" );

      masterPeId = ( masterPeId + 1 )%numPes;
   }

   pe = createPE ( _defArch, getBindingId( masterPeId ), masterPeId );
   pe->setNUMANode( getNodeOfPE( pe->getId() ) );
   _pes.push_back ( pe );
   _workers.push_back( &pe->associateThisThread ( getUntieMaster() ) );
   CPU_SET( getBindingId( masterPeId ), &_cpuActiveSet );

   WD &mainWD = *myThread->getCurrentWD();
   (void) mainWD.getDirectory(true);

   if ( _pmInterface->getInternalDataSize() > 0 ) {
      char *data = NEW char[_pmInterface->getInternalDataSize()];
      _pmInterface->initInternalData( data );
      mainWD.setInternalData( data );
   }

   _pmInterface->setupWD( mainWD );

   /* Renaming currend thread as Master */
   myThread->rename("Master");

   NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseOpenStateEvent (NANOS_STARTUP) );

   // For each plugin, notify it's the way to reserve PEs if they are required
   for ( ArchitecturePlugins::const_iterator it = _archs.begin();
        it != _archs.end(); ++it )
   {
      (*it)->createBindingList();
   }
   // Right now, _bindings should only store SMP PEs ids

   // Create PEs
   int p;
   for ( p = 1 + _enableDAST; p < numPes ; p++ ) {
      pe = createPE ( "smp", getBindingId( p ), p );
      pe->setNUMANode( getNodeOfPE( pe->getId() ) );
      _pes.push_back ( pe );

      CPU_SET( getBindingId( p ), &_cpuActiveSet );
   }

   // Create threads
   for ( int ths = 1; ths < _numThreads; ths++ ) {
      pe = _pes[ ( ths + ( int )( _enableDAST ) ) % numPes ];
      _workers.push_back( &pe->startWorker() );
   }

   // For each plugin create PEs and workers
   //! \bug  FIXME (#855)
   for ( ArchitecturePlugins::const_iterator it = _archs.begin();
        it != _archs.end(); ++it )
   {
      for ( unsigned archPE = 0; archPE < (*it)->getNumPEs(); ++archPE )
      {
         PE * processor = (*it)->createPE( archPE, p );
         fatal_cond0( processor == NULL, "ArchPlugin::createPE returned NULL" );
         _pes.push_back( processor );
         _workers.push_back( &processor->startWorker() );
         CPU_SET( processor->getId(), &_cpuActiveSet );
         ++p;
      }
   }

   // Set up internal data for each worker
   for ( ThreadList::const_iterator it = _workers.begin(); it != _workers.end(); it++ ) {

      WD & threadWD = (*it)->getThreadWD();
      if ( _pmInterface->getInternalDataSize() > 0 ) {
         char *data = NEW char[_pmInterface->getInternalDataSize()];
         _pmInterface->initInternalData( data );
         threadWD.setInternalData( data );
      }
      _pmInterface->setupWD( threadWD );
   }

#ifdef SPU_DEV
   PE *spu = NEW nanos::ext::SPUProcessor(100, (nanos::ext::SMPProcessor &) *_pes[0]);
   spu->startWorker();
#endif


   if ( getSynchronizedStart() ) {
      // Wait for the rest of the gang to be ready, but do not yet notify master thread is ready
      while (_initializedThreads.value() < ( _targetThreads - 1 ) ) {}
   }

   // FIXME (855): do this before thread creation, after PE creation
   completeNUMAInfo();

   switch ( getInitialMode() )
   {
      case POOL:
         verbose0("Pool model enabled (OmpSs)");
         _mainTeam = createTeam( _workers.size(), /*constraints*/ NULL, /*reuse*/ true, /*enter*/ true, /*parallel*/ false );
         break;
      case ONE_THREAD:
         verbose0("One-thread model enabled (OpenMP)");
         _mainTeam = createTeam( 1, /*constraints*/ NULL, /*reuse*/ true, /*enter*/ true, /*parallel*/ true );
         break;
      default:
         fatal("Unknown initial mode!");
         break;
   }

   NANOS_INSTRUMENT ( static InstrumentationDictionary *ID = sys.getInstrumentation()->getInstrumentationDictionary(); )
   NANOS_INSTRUMENT ( static nanos_event_key_t num_threads_key = ID->getEventKey("set-num-threads"); )
   NANOS_INSTRUMENT ( nanos_event_value_t team_size =  (nanos_event_value_t) myThread->getTeam()->size(); )
   NANOS_INSTRUMENT ( sys.getInstrumentation()->raisePointEvents(1, &num_threads_key, &team_size); )

   /* Master thread is ready now */
   if ( getSynchronizedStart() )
     threadReady();

   // Paused threads: set the condition checker
   _pausedThreadsCond.setConditionChecker( EqualConditionChecker<unsigned int >( &_pausedThreads.override(), _workers.size() ) );
   _unpausedThreadsCond.setConditionChecker( EqualConditionChecker<unsigned int >( &_pausedThreads.override(), 0 ) );

   // All initialization is ready, call postInit hooks
   const OS::InitList & externalInits = OS::getPostInitializationFunctions();
   std::for_each(externalInits.begin(),externalInits.end(), ExecInit());

   NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseCloseStateEvent() );
   NANOS_INSTRUMENT ( sys.getInstrumentation()->raiseOpenStateEvent (NANOS_RUNNING) );

   // List unrecognised arguments
   std::string unrecog = Config::getOrphanOptions();
   if ( !unrecog.empty() )
      warning( "Unrecognised arguments: " << unrecog );
   Config::deleteOrphanOptions();

   // hwloc can be now unloaded
   if ( isHwlocAvailable() )
      unloadHwloc();

   if ( _summary )
      environmentSummary();
}
