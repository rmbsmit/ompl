/*********************************************************************
*  @copyright Software License Agreement (BSD License)
*  Copyright (c) 2013, Rutgers the State University of New Jersey, New Brunswick 
*  All Rights Reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Rutgers University nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Andrew Dobson */

#ifndef OMPL_GEOMETRIC_PLANNERS_SPARS_TWO_
#define OMPL_GEOMETRIC_PLANNERS_SPARS_TWO_

#include "ompl/geometric/planners/PlannerIncludes.h"
#include "ompl/datastructures/NearestNeighbors.h"
#include "ompl/geometric/PathSimplifier.h"
#include "ompl/util/Time.h"

#include <boost/range/adaptor/map.hpp>
#include <boost/unordered_map.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/pending/disjoint_sets.hpp>
#include <boost/function.hpp>
#include <iostream>
#include <fstream>
#include <utility>
#include <vector>
#include <map>

namespace ompl
{

    namespace geometric
    {

        /**
           @anchor gSPARStwo
           @par Short description
           SPARStwo is a variant of the SPARS algorithm which removes the
           dependency on having the dense graph, D.  It works through similar
           mechanics, but uses a different approach to identifying interfaces
           and computing shortest paths through said interfaces.
           @par External documentation
           A. Dobson, K. Bekris,
           Improving Sparse Roadmap Spanners,
           <em>IEEE International Conference on Robotics and Automation (ICRA)</em> May 2013.
           <a href="http://www.cs.rutgers.edu/~kb572/pubs/spars2.pdf">[PDF]</a>
        */

        /** \brief <b> SPArse Roadmap Spanner Version 2.0 </b> */
        class SPARStwo : public base::Planner
        {
        public:
            /** \brief Enumeration which specifies the reason a guard is added to the spanner. */
            enum GuardType
            {
                START,
                GOAL,
                COVERAGE,
                CONNECTIVITY,
                INTERFACE,
                QUALITY,
            };
            
        protected:
            /** \brief containers for states which keep NULL safety at all times */
            class safeState
            {
            public:
                /** \brief State which this object keeps in a safe state. */
                base::State* st_;
                
                safeState( void )
                {
                    st_ = NULL;
                }
                
                /** \brief Parameterized constructor which takes a state. */
                safeState( base::State* state )
                {
                    st_ = state;
                }
                
                /** \brief Assignment of a state. */
                void operator=( base::State* state )
                {
                    st_ = state;
                }
                
                /** \brief Retrieval method for an actual state. */
                base::State* get( void )
                {
                    return st_;
                }
                
                /** \brief Const retrieval method for an actual state. */
                const base::State* get( void ) const
                {
                    return st_;
                }
                
                /** \brief Sets the internal state pointer to NULL */
                void setNull( void )
                {
                    st_ = NULL;
                }
            };
            
            /** \brief Pair of safe states which support an interface. */
            typedef std::pair< safeState, safeState > safeStatePair;
            /** \brief Pair of vertices which support an interface. */
            typedef std::pair< unsigned long, unsigned long > VertexPair;
            
        public:
            /** \brief Interface information storage class, which does bookkeeping for criterion four. */
            struct InterfaceData
            {
                /** \brief States which lie inside the visibility region of a vertex and support an interface. */
                safeStatePair points_;
                /** \brief States which lie just outside the visibility region of a vertex and support an interface. */
                safeStatePair sigmas_;
                /** \brief Last known distance between the two interfaces supported by points_ and sigmas_. */
                double      d_;
                
                /** \brief Constructor */
                InterfaceData( void )
                {
                    d_ = std::numeric_limits<double>::infinity();
                }
                
                /** \brief Sets information for the first interface (i.e. interface with smaller index vertex). */
                void setFirst( const safeState& p, const safeState& s, const base::SpaceInformationPtr& si )
                {
                    if( points_.first.get() != NULL )
                        si->freeState( points_.first.get() );
                    points_.first = si->cloneState( p.get() );
                    if( sigmas_.first.get() != NULL )
                        si->freeState( sigmas_.first.get() );
                    sigmas_.first = si->cloneState( s.get() );
                    
                    if( points_.second.get() != NULL )
                    {
                        d_ = si->distance( points_.first.get(), points_.second.get() );
                    }
                }
                
                /** \brief Sets information for the second interface (i.e. interface with larger index vertex). */
                void setSecond( const safeState& p, const safeState& s, const base::SpaceInformationPtr& si )
                {
                    if( points_.second.get() != NULL )
                        si->freeState( points_.second.get() );
                    points_.second = si->cloneState( p.get() );
                    if( sigmas_.second.get() != NULL )
                        si->freeState( sigmas_.second.get() );
                    sigmas_.second = si->cloneState( s.get() );
                    
                    if( points_.first.get() != NULL )
                    {
                        d_ = si->distance( points_.first.get(), points_.second.get() );
                    }
                }
                
            };
            
        protected:
            /** \brief the hash which maps pairs of neighbor points to pairs of states */
            typedef boost::unordered_map< VertexPair, InterfaceData, boost::hash< VertexPair > > InterfaceHash;

        public:

            struct vertex_state_t {
                typedef boost::vertex_property_tag kind;
            };
            
            struct vertex_color_t {
                typedef boost::vertex_property_tag kind;
            };
            
            struct vertex_interface_data_t {
                typedef boost::vertex_property_tag kind;
            };
            
            /**
             @brief The underlying roadmap graph.

             @par Any BGL graph representation could be used here. Because we
             expect the roadmap to be sparse (m<n^2), an adjacency_list is more
             appropriate than an adjacency_matrix.

             @par Obviously, a ompl::base::State* vertex property is required.
             The incremental connected components algorithm requires
             vertex_predecessor_t and vertex_rank_t properties.
             If boost::vecS is not used for vertex storage, then there must also
             be a boost:vertex_index_t property manually added.

             @par Edges should be undirected and have a weight property.
             */
            typedef boost::adjacency_list <
                boost::vecS, boost::vecS, boost::undirectedS,
                boost::property < vertex_state_t, base::State*,
                boost::property < boost::vertex_predecessor_t, unsigned long int,
                boost::property < boost::vertex_rank_t, unsigned long int,
                boost::property < vertex_color_t, unsigned int,
                boost::property < vertex_interface_data_t, InterfaceHash > > > > >,
                boost::property < boost::edge_weight_t, double >
            > Graph;

            typedef boost::graph_traits<Graph>::vertex_descriptor Vertex;
            typedef boost::graph_traits<Graph>::edge_descriptor   Edge;

            /** @brief A function returning the milestones that should be
             * attempted to connect to
             *
             * @note Can't use the prefered boost::function syntax here because
             * the Python bindings don't like it.
             */
            typedef boost::function<std::vector<Vertex>&(const Vertex)>
                ConnectionStrategy;

            /** \brief Constructor */
            SPARStwo(const base::SpaceInformationPtr &si );
            /** \brief Destructor */
            virtual ~SPARStwo(void);

            virtual void setProblemDefinition(const base::ProblemDefinitionPtr &pdef);

            /** \brief Sets the stretch factor */
            void setStretchFactor( double t )
            {
                stretchFactor_ = t;
            }
            
            /** \brief Sets vertex visibility range */
            void setSparseDelta( double D )
            {
                sparseDelta_ = D;
            }
            
            /** \brief Sets interface support tolerance */
            void setDenseDelta( double d )
            {
                denseDelta_ = d;
            }
            
            /** \brief Sets the maximum failures until termination */
            void setMaxFailures( unsigned int m )
            {
                maxFailures_ = m;
            }
            
            /** \brief Retrieve the maximum consecutive failure limit. */
            unsigned getMaxFailures( ) const
            {
                return maxFailures_;
            }
            
            /** \brief Retrieve the dense graph interface support delta. */
            double getDenseDelta( ) const
            {
                return denseDelta_;
            }
            
            /** \brief Retrieve the sparse graph visibility range delta. */
            double getSparseDelta( ) const
            {
                return sparseDelta_;
            }
            
            /** \brief Retrieve the spanner's set stretch factor. */
            double getStretchFactor( ) const
            {
                return stretchFactor_;
            }


            virtual void getPlannerData(base::PlannerData &data) const;
            
            /** \brief Function that can solve the motion planning
                problem. This function can be called multiple times on
                the same problem, without calling clear() in
                between. This allows the planner to continue work for
                more time on an unsolved problem, for example. Start
                and goal states from the currently specified
                ProblemDefinition are cached. This means that between
                calls to solve(), input states are only added, not
                removed. When using PRM as a multi-query planner, the
                input states should be however cleared, without
                clearing the roadmap itself. This can be done using
                the clearQuery() function. */
            virtual base::PlannerStatus solve(const base::PlannerTerminationCondition &ptc);

            /** \brief Alternate solve call with maximum failures as a 
                function parameter.  Overwrites the parameter member maxFailures_. */
            virtual base::PlannerStatus solve(const base::PlannerTerminationCondition &ptc, unsigned int maxFail );

            /** \brief Clear the query previously loaded from the ProblemDefinition.
                Subsequent calls to solve() will reuse the previously computed roadmap,
                but will clear the set of input states constructed by the previous call to solve().
                This enables multi-query functionality for PRM. */
            void clearQuery(void);

            virtual void clear(void);

            /** \brief Set a different nearest neighbors datastructure */
            template<template<typename T> class NN>
            void setNearestNeighbors(void)
            {
                nn_.reset(new NN< Vertex >());
                connectionStrategy_.clear();
                if (isSetup())
                    setup();
            }

            virtual void setup(void);

            /** \brief Retrieve the computed roadmap. */
            const Graph& getRoadmap(void) const
            {
                return g_;
            }

            /** \brief Get the number of vertices in the sparse roadmap. */
            unsigned int milestoneCount(void) const
            {
                return boost::num_vertices(g_);
            }
            
        protected:
            /** \brief Roadmap Nearest Neighbors structure. */
            typedef boost::shared_ptr< NearestNeighbors<Vertex> > RoadmapNeighbors;
            
            /** \brief Sample a valid random state, storing it in qNew_ (and returning it) */
            virtual base::State* sample( void );
            
            /** \brief Free all the memory allocated by the planner */
            void freeMemory(void);

            /** \brief Checks to see if the sample needs to be added to ensure coverage of the space */
            bool checkAddCoverage( void );
            
            /** \brief Checks to see if the sample needs to be added to ensure connectivity */
            bool checkAddConnectivity( void );

            /** \brief Checks to see if the current sample reveals the existence of an interface, and if so, tries to bridge it. */
            bool checkAddInterface( void );
            
            /** \brief Checks vertex v for short paths through its region and adds when appropriate. */
            bool checkAddPath( Vertex v );
            
            /** \brief A reset function for resetting the failures count */
            void resetFailures( void );
            
            /** \brief Finds visible nodes in the graph near st */
            void findGraphNeighbors(  base::State* st );
            
            /** \brief Approaches the graph from a given vertex */
            void approachGraph( Vertex v );
            
            /** \brief Finds the representative of the input state, st  */
            void findGraphRepresentative( base::State* st );
            
            /** \brief Finds representatives of samples near qNew_ which are not his representative */
            void findCloseRepresentatives();
            
            /** \brief High-level method which updates pair point information for repV_ with neighbor r */
            void updatePairPoints( Vertex rep, const safeState& q, Vertex r, const safeState& s );

            /** \brief Computes all nodes which qualify as a candidate v" for v and vp */
            void computeVPP( Vertex v, Vertex vp );

            /** \brief Computes all nodes which qualify as a candidate x for v, v', and v" */
            void computeX( Vertex v, Vertex vp, Vertex vpp );

            /** \brief Rectifies indexing order for accessing the vertex data */
            VertexPair index( Vertex vp, Vertex vpp );
            
            /** \brief Retrieves the Vertex data associated with v,vp,vpp */
            InterfaceData& getData( Vertex v, Vertex vp, Vertex vpp );
            
            void setData( Vertex v, Vertex vp, Vertex vpp, const InterfaceData& d );
            
            /** \brief Performs distance checking for the candidate new state, q against the current information */
            void distanceCheck( Vertex rep, const safeState& q, Vertex r, const safeState& s, Vertex rp );

            /** \brief When a new guard is added at state st, finds all guards who must abandon their interface information and deletes that information */
            void abandonLists( base::State* st );
            
            /** \brief Deletes all the states in a vertex's lists */
            void deletePairInfo( Vertex v );
            
            /** \brief Construct a guard for a given state (\e state) and store it in the nearest neighbors data structure */
            virtual Vertex addGuard( base::State *state, GuardType type);

            /** \brief Connect two guards in the roadmap */
            virtual void connect( Vertex v, Vertex vp );
            
            /** \brief Make two milestones (\e m1 and \e m2) be part of the same connected component. The component with fewer elements will get the id of the component with more elements. */
            void uniteComponents(Vertex m1, Vertex m2);

            /** \brief Check if there exists a solution, i.e., there exists a pair of milestones such that the first is in \e start and the second is in \e goal, and the two milestones are in the same connected component. If a solution is found, the path is saved. */
            bool haveSolution(const std::vector<Vertex> &start, const std::vector<Vertex> &goal, base::PathPtr &solution);

            /** \brief Returns the value of the addedSolution_ member. */
            bool addedNewSolution (void) const;

            /** \brief Returns whether we have reached the iteration failures limit, maxFailures_ */
            bool reachedFailureLimit (void) const;
            
            /** \brief Given two milestones from the same connected component, construct a path connecting them and set it as the solution */
            virtual base::PathPtr constructSolution(const Vertex start, const Vertex goal) const;

            /** \brief Sampler user for generating valid samples in the state space */
            base::ValidStateSamplerPtr                                          sampler_;

            /** \brief Sampler user for generating random in the state space */
            base::StateSamplerPtr                                               simpleSampler_;

            /** \brief Nearest neighbors data structure */
            RoadmapNeighbors                                                    nn_;

            /** \brief Connectivity graph */
            Graph                                                               g_;

            /** \brief Array of start milestones */
            std::vector<Vertex>                                                 startM_;

            /** \brief Array of goal milestones */
            std::vector<Vertex>                                                 goalM_;
            
            /** \brief Vertex for performing nearest neighbor queries. */
            Vertex                                                              queryVertex_;
            
            /** \brief Stretch Factor as per graph spanner literature (multiplicative bound on path quality) */
            double                                                              stretchFactor_;

            /** \brief Maximum visibility range for nodes in the graph */
            double                                                              sparseDelta_;

            /** \brief Maximum range for allowing two samples to support an interface */
            double                                                              denseDelta_;

            /** \brief The number of consecutive failures to add to the graph before termination */
            unsigned int                                                        maxFailures_;
            
            /** \brief Number of sample points to use when trying to detect interfaces. */
            unsigned int                                                        nearSamplePoints_;
            
            /** \brief A pointer to the most recent sample we have come up with */
            base::State*                                                        qNew_;
            
            /** \brief A pointer holding a temporary state used for additional sampling processes */
            base::State*                                                        holdState_;
            
            /** \brief The whole neighborhood set which has been most recently computed */
            std::vector< Vertex >                                               graphNeighborhood_;
            
            /** \brief The visible neighborhood set which has been most recently computed */
            std::vector< Vertex >                                               visibleNeighborhood_;
            
            /** \brief The representatives of nodes near a sample.  Filled by getCloseRepresentatives(). */
            std::pair< std::vector< Vertex >, std::vector< base::State* > >     closeRepresentatives_;
            
            /** \brief Candidate v" vertices as described in the method, filled by function computeVPP(). */
            std::vector< Vertex >                                               VPPs_;
            /** \brief Candidate x vertices as described in the method, filled by function computeX(). */
            std::vector< Vertex >                                               Xs_;
            
            /** \brief A holder to remember who qNew_'s representative in the graph is. */
            Vertex                                                              repV_;
            
            /** \brief Access to the internal base::state at each Vertex */
            boost::property_map<Graph, vertex_state_t>::type                    stateProperty_;

            /** \brief A path simplifier used to simplify dense paths added to the graph */
            PathSimplifierPtr                                                   psimp_;

            /** \brief Access to the weights of each Edge */
            boost::property_map<Graph, boost::edge_weight_t>::type              weightProperty_;
            
            /** \brief Access to the colors for the vertices */
            boost::property_map<Graph, vertex_color_t>::type                    colorProperty_;
            
            /** \brief Access to the interface pair information for the vertices */
            boost::property_map<Graph, vertex_interface_data_t>::type           interfaceDataProperty_;

            /** \brief Data structure that maintains the connected components */
            boost::disjoint_sets<
                boost::property_map<Graph, boost::vertex_rank_t>::type,
                boost::property_map<Graph, boost::vertex_predecessor_t>::type >
                                                                                disjointSets_;

            /** \brief Function that returns the milestones to attempt connections with */
            ConnectionStrategy                                                  connectionStrategy_;

            /** \brief Random number generator */
            RNG                                                                 rng_;

            /** \brief A flag indicating that a solution has been added during solve() */
            bool                                                                addedSolution_;

            /** \brief Mutex to guard access to the Graph member (g_) */
            mutable boost::mutex                                                graphMutex_;
            
            /** \brief A counter for the number of iterations of the algorithm */
            unsigned int                                                        iterations_;
            
        private:
            /** \brief Clears the given interface data. */
            void clearInterfaceData( InterfaceData& iData, const base::SpaceInformationPtr& si )
            {
                clearSafeState( iData.points_.first, si );
                clearSafeState( iData.points_.second, si );
                clearSafeState( iData.sigmas_.first, si );
                clearSafeState( iData.sigmas_.second, si );
                iData.d_ = std::numeric_limits<double>::infinity();
            }

            /** \brief Deallocates the internal state of the safe state and sets it to NULL. */
            void clearSafeState( safeState& ss, const base::SpaceInformationPtr& si )
            {
                if( ss.get() != NULL )
                    si->freeState( ss.get() );
                ss.setNull();
            }

            /** \brief Compute distance between two milestones (this is simply distance between the states of the milestones) */
            double distanceFunction(const Vertex a, const Vertex b) const
            {
                return si_->distance(stateProperty_[a], stateProperty_[b]);
            }

        };

    }
}

#endif