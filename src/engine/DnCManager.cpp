/*********************                                                        */
/*! \file DnCManager.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Haoze Wu
 ** This file is part of the Marabou project.
 ** Copyright (c) 2017-2019 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved. See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** [[ Add lengthier description here ]]

 **/

#include "AcasParser.h"
#include "Debug.h"
#include "DivideStrategy.h"
#include "DnCManager.h"
#include "DnCWorker.h"
#include "LargestIntervalDivider.h"
#include "MStringf.h"
#include "PiecewiseLinearCaseSplit.h"
#include "PropertyParser.h"
#include "QueryDivider.h"
#include "MarabouError.h"
#include "TimeUtils.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

void DnCManager::dncSolve( WorkerQueue *workload, std::shared_ptr<Engine> engine,
                           std::atomic_uint &numUnsolvedSubQueries,
                           std::atomic_bool &shouldQuitSolving,
                           unsigned threadId, unsigned onlineDivides,
                           float timeoutFactor, DivideStrategy divideStrategy )
{
    log( Stringf( "Thread #%u on CPU %u", threadId, sched_getcpu() ) );
    DnCWorker worker( workload, engine, std::ref( numUnsolvedSubQueries ),
                      std::ref( shouldQuitSolving ), threadId, onlineDivides,
                      timeoutFactor, divideStrategy );
    worker.run();
}

DnCManager::DnCManager( unsigned numWorkers, unsigned initialDivides,
                        unsigned initialTimeout, unsigned onlineDivides,
                        float timeoutFactor, DivideStrategy divideStrategy,
                        String networkFilePath, String propertyFilePath,
                        unsigned verbosity )
    : _numWorkers( numWorkers )
    , _initialDivides( initialDivides )
    , _initialTimeout( initialTimeout )
    , _onlineDivides( onlineDivides )
    , _timeoutFactor( timeoutFactor )
    , _divideStrategy( divideStrategy )
    , _networkFilePath( networkFilePath )
    , _propertyFilePath( propertyFilePath )
    , _exitCode( DnCManager::NOT_DONE )
    , _workload( NULL )
    , _timeoutReached( false )
    , _numUnsolvedSubQueries( 0 )
    , _verbosity( verbosity )
    , _initializedWithQuery(false)
{
}

DnCManager::DnCManager(unsigned numWorkers, unsigned initialDivides,
        unsigned initialTimeout, unsigned onlineDivides, float timeoutFactor,
        DivideStrategy divideStrategy, InputQuery initialQuery,
        unsigned verbosity)
  : _numWorkers( numWorkers )
  , _initialDivides( initialDivides )
  , _initialTimeout( initialTimeout )
  , _onlineDivides( onlineDivides )
  , _timeoutFactor( timeoutFactor )
  , _divideStrategy( divideStrategy )
  , _networkFilePath( "" )
  , _propertyFilePath( "" )
  , _exitCode( DnCManager::NOT_DONE )
  , _workload( NULL )
  , _timeoutReached( false )
  , _numUnsolvedSubQueries( 0 )
  , _verbosity( verbosity )
  , _initializedWithQuery(true) {

    // Create the base engine
    _baseEngine = std::make_shared<Engine>();
    if ( _baseEngine->processInputQuery( initialQuery ) ) {

        // Create engines for each thread
        for ( unsigned i = 0; i < _numWorkers; ++i )
        {
            auto engine = std::make_shared<Engine>( _verbosity );
            InputQuery *inputQuery = new InputQuery();
            *inputQuery = initialQuery;
            engine->processInputQuery( *inputQuery );
            _engines.append( engine );
        }

    } else {
        _exitCode = DnCManager::UNSAT;
    }
}

DnCManager::~DnCManager()
{
    freeMemoryIfNeeded();
}

void DnCManager::freeMemoryIfNeeded()
{
    if ( _workload )
    {
        SubQuery *subQuery;
        while ( !_workload->empty() )
        {
            _workload->pop( subQuery );
            delete subQuery;
        }

        delete _workload;
        _workload = NULL;
    }
}

void DnCManager::solve( unsigned timeoutInSeconds )
{
    enum {
        MICROSECONDS_IN_SECOND = 1000000
    };

    unsigned long long timeoutInMicroSeconds = timeoutInSeconds * MICROSECONDS_IN_SECOND;
    struct timespec startTime = TimeUtils::sampleMicro();

    // Preprocess the input query and create an engine for each of the threads
    if ( !_initializedWithQuery && !createEngines() )
    {
        _exitCode = DnCManager::UNSAT;
        printResult();
        return;
    }
    if (_exitCode == DnCManager::UNSAT) {
      return;
    }

    // Prepare the mechanism through which we can ask the engines to quit
    List<std::atomic_bool *> quitThreads;
    for ( unsigned i = 0; i < _numWorkers; ++i )
        quitThreads.append( _engines[i]->getQuitRequested() );

    // Partition the input query into initial subqueries, and place these
    // queries in the queue
    _workload = new WorkerQueue( 0 );
    if ( !_workload )
        throw MarabouError( MarabouError::ALLOCATION_FAILED, "DnCManager::workload" );

    SubQueries subQueries;
    initialDivide( subQueries );

    // Create objects shared across workers
    _numUnsolvedSubQueries = subQueries.size();
    std::atomic_bool shouldQuitSolving( false );
    WorkerQueue *workload = new WorkerQueue( 0 );
    for ( auto &subQuery : subQueries )
    {
        if ( !workload->push( subQuery ) )
        {
            // This should never happen
            ASSERT( false );
        }
    }

    // Spawn threads and start solving
    std::list<std::thread> threads;
    for ( unsigned threadId = 0; threadId < _numWorkers; ++threadId )
    {
        threads.push_back( std::thread( dncSolve, workload,
                                        _engines[ threadId ],
                                        std::ref( _numUnsolvedSubQueries ),
                                        std::ref( shouldQuitSolving ),
                                        threadId, _onlineDivides,
                                        _timeoutFactor, _divideStrategy ) );
    }

    // Wait until either all subQueries are solved or a satisfying assignment is
    // found by some worker
    while ( _numUnsolvedSubQueries.load() > 0 &&
            !shouldQuitSolving.load() &&
            !_timeoutReached )
    {
        updateTimeoutReached( startTime, timeoutInMicroSeconds );
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }

    // Now that we are done, tell all workers to quit
    for ( auto &quitThread : quitThreads )
        *quitThread = true;

    for ( auto &thread : threads )
        thread.join();

    updateDnCExitCode();
    printResult();
    return;
}

DnCManager::DnCExitCode DnCManager::getExitCode() const
{
    return _exitCode;
}

void DnCManager::updateDnCExitCode()
{
    bool hasSat = false;
    bool hasError = false;
    bool hasQuitRequested = false;
    for ( auto &engine : _engines )
    {
        Engine::ExitCode result = engine->getExitCode();
        if ( result == Engine::SAT )
        {
            _engineWithSATAssignment = engine;
            hasSat = true;
            break;
        }
        else if ( result == Engine::ERROR )
            hasError = true;
        else if ( result == Engine::QUIT_REQUESTED )
            hasQuitRequested = true;
    }
    if ( hasSat )
        _exitCode = DnCManager::SAT;
    else if ( _timeoutReached )
        _exitCode = DnCManager::TIMEOUT;
    else if ( hasQuitRequested )
        _exitCode = DnCManager::QUIT_REQUESTED;
    else if ( hasError )
        _exitCode = DnCManager::ERROR;
    else if ( _numUnsolvedSubQueries.load() == 0 )
        _exitCode = DnCManager::UNSAT;
    else
    {
        ASSERT( false ); // This should never happen
        _exitCode = DnCManager::NOT_DONE;
    }
}

String DnCManager::getResultString()
{
    switch ( _exitCode )
    {
    case DnCManager::SAT:
        return "SAT";
    case DnCManager::UNSAT:
        return "UNSAT";
    case DnCManager::ERROR:
        return "ERROR";
    case DnCManager::NOT_DONE:
        return "NOT_DONE";
    case DnCManager::QUIT_REQUESTED:
        return "QUIT_REQUESTED";
    case DnCManager::TIMEOUT:
        return "TIMEOUT";
    default:
        ASSERT( false );
        return "";
    }
}

void DnCManager::printResult()
{
    switch ( _exitCode )
    {
    case DnCManager::SAT:
    {
        std::cout << "DnCManager::solve SAT query" << std::endl;

        ASSERT( _engineWithSATAssignment != nullptr );

        InputQuery *inputQuery = _engineWithSATAssignment->getInputQuery();
        _engineWithSATAssignment->extractSolution( *( inputQuery ) );


        double inputs[inputQuery->getNumInputVariables()];
        double outputs[inputQuery->getNumOutputVariables()];
        printf( "Input assignment:\n" );
        for ( unsigned i = 0; i < inputQuery->getNumInputVariables(); ++i )
        {
            printf( "\tx%u = %lf\n", i, inputQuery->getSolutionValue( inputQuery->inputVariableByIndex( i ) ) );
            inputs[i] = inputQuery->getSolutionValue( inputQuery->inputVariableByIndex( i ) );
        }

        _engineWithSATAssignment->getInputQuery()->getNetworkLevelReasoner()
            ->evaluate( inputs, outputs );

        printf( "\n" );
        printf( "Output:\n" );
        for ( unsigned i = 0; i < inputQuery->getNumOutputVariables(); ++i )
            printf( "\ty%u = %lf\n", i, outputs[i] );
        printf( "\n" );
        break;
    }
    case DnCManager::UNSAT:
        std::cout << "DnCManager::solve UNSAT query" << std::endl;
        break;
    case DnCManager::ERROR:
        std::cout << "DnCManager::solve ERROR" << std::endl;
        break;
    case DnCManager::NOT_DONE:
        std::cout << "DnCManager::solve NOT_DONE" << std::endl;
        break;
    case DnCManager::QUIT_REQUESTED:
        std::cout << "DnCManager::solve QUIT_REQUESTED" << std::endl;
        break;
    case DnCManager::TIMEOUT:
        std::cout << "DnCManager::solve TIMEOUT" << std::endl;
        break;
    default:
        ASSERT( false );
    }
}

bool DnCManager::createEngines()
{
    // Create the base engine
    _baseEngine = std::make_shared<Engine>();
    InputQuery *baseInputQuery = new InputQuery();

    // InputQuery is owned by engine
    AcasParser acasParser( _networkFilePath );
    acasParser.generateQuery( *baseInputQuery );

    if ( _propertyFilePath != "" )
        PropertyParser().parse( _propertyFilePath, *baseInputQuery );

    if ( !_baseEngine->processInputQuery( *baseInputQuery ) )
        // Solved by preprocessing, we are done!
        return false;

    // Create engines for each thread
    for ( unsigned i = 0; i < _numWorkers; ++i )
    {
        auto engine = std::make_shared<Engine>( _verbosity );
        InputQuery *inputQuery = new InputQuery();
        *inputQuery = *baseInputQuery;
        engine->processInputQuery( *inputQuery );
        _engines.append( engine );
    }

    return true;
}

void DnCManager::initialDivide( SubQueries &subQueries )
{
    const List<unsigned> inputVariables( _baseEngine->getInputVariables() );
    std::unique_ptr<QueryDivider> queryDivider = nullptr;
    if ( _divideStrategy == DivideStrategy::LargestInterval )
    {
        queryDivider = std::unique_ptr<QueryDivider>
            ( new LargestIntervalDivider( inputVariables ) );
    }
    else
    {
        // Default
        queryDivider = std::unique_ptr<QueryDivider>
            ( new LargestIntervalDivider( inputVariables ) );
    }

    String queryId = "";
    // Create a new case split
    QueryDivider::InputRegion initialRegion;
    InputQuery *inputQuery = _baseEngine->getInputQuery();
    for ( const auto &variable : inputVariables )
    {
        initialRegion._lowerBounds[variable] =
            inputQuery->getLowerBounds()[variable];
        initialRegion._upperBounds[variable] =
            inputQuery->getUpperBounds()[variable];
    }

    auto split = std::unique_ptr<PiecewiseLinearCaseSplit>
        ( new PiecewiseLinearCaseSplit() );

    // Add bound as equations for each input variable
    for ( const auto &variable : inputVariables )
    {
        double lb = initialRegion._lowerBounds[variable];
        double ub = initialRegion._upperBounds[variable];
        split->storeBoundTightening( Tightening( variable, lb,
                                                 Tightening::LB ) );
        split->storeBoundTightening( Tightening( variable, ub,
                                                 Tightening::UB ) );
    }

    queryDivider->createSubQueries( pow( 2, _initialDivides ), queryId,
                                    *split, _initialTimeout, subQueries );
}

void DnCManager::updateTimeoutReached( timespec startTime, unsigned long long
                                       timeoutInMicroSeconds )
{
    if ( timeoutInMicroSeconds == 0 )
        return;
    struct timespec now = TimeUtils::sampleMicro();
    _timeoutReached = TimeUtils::timePassed( startTime, now ) >=
        timeoutInMicroSeconds;
}

void DnCManager::log( const String &message )
{
    if ( GlobalConfiguration::DNC_MANAGER_LOGGING )
        printf( "DnCManager: %s\n", message.ascii() );
}

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//
