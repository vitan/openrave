// -*- Coding: utf-8 -*-
// Copyright (C) 2014 Alejandro Perez & Rosen Diankov
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
/// \author Alejandro Perez & Rosen Diankov
#include "configurationcachetree.h"

#include <sstream>
#include <boost/lexical_cast.hpp>

// High
// find a way to lose the map for children

//Low

//KinBodyPtr body;
//std::vector<dReal> values;
//std::vector<uint8_t> venablestates;
//body->GetConfigurationValues(values);
//body->GetLinkEnables(venablestates);

//std::map<KinBodyPtr, std::pair<std::vector<dReal>, std::vector<uint8_t> > > mapEnvironmentState;
namespace configurationcache {

inline dReal Sqr(dReal x) {
    return x*x;
}

CacheTreeNode::CacheTreeNode(const std::vector<dReal>& cs, Vector* plinkspheres)
{
    std::copy(cs.begin(), cs.end(), _pcstate);
    _plinkspheres = plinkspheres;
//    _approxdispersion.first = CacheTreeNodePtr();
//    _approxdispersion.second = std::numeric_limits<float>::infinity();
//    _approxnn.first = CacheTreeNodePtr();
//    _approxnn.second = std::numeric_limits<float>::infinity();
    _conftype = CNT_Unknown;
    _robotlinkindex = -1;
    _level = 0;
    _hasselfchild = 0;
    _usenn = 1;
}

CacheTreeNode::CacheTreeNode(const dReal* pstate, int dof, Vector* plinkspheres)
{
    std::copy(pstate, pstate+dof, _pcstate);
    _plinkspheres = plinkspheres;
    _conftype = CNT_Unknown;
    _robotlinkindex = -1;
    _level = 0;
    _hasselfchild = 0;
    _usenn = 1;
}

void CacheTreeNode::SetCollisionInfo(CollisionReportPtr report)
{
    if(!!report && report->numCols > 0) {
        _collidinglinktrans = report->plink1->GetTransform();
        _robotlinkindex = report->plink1->GetIndex();
        _collidinglink = report->plink2;
        _conftype = CNT_Collision;
    }
    else {
        _conftype = CNT_Free;
        _collidinglink.reset();
        _robotlinkindex = -1;
    }
}

//void CacheTreeNode::UpdateApproximates(dReal distance, CacheTreeNodePtr v)
//{
//    // if both are same type, update nn
//    if (v->GetType() == _conftype) {
//        if (distance < _approxnn.second) {
//            _approxnn.first = v;
//            _approxnn.second = distance;
//        }
//
//    }
//    // if they are different types, update dispersion
//    else{
//        if ( distance < _approxdispersion.second) {
//            _approxdispersion.first = v;
//            //dReal oldd = _approxdispersion.second;
//            //RAVELOG_DEBUG_FORMAT("dispersion %f to %f\n",oldd%distance);
//            _approxdispersion.second = distance;
//        }
//    }
//}

CacheTree::CacheTree(int statedof) : _poolNodes(sizeof(CacheTreeNode)+sizeof(dReal)*statedof)
{
    _statedof=statedof;
    _weights.resize(_statedof, 1.0);
    Init(_weights, 1);
}

CacheTree::~CacheTree()
{
    Reset();
    _weights.clear();
}

void CacheTree::Init(const std::vector<dReal>& weights, dReal maxdistance)
{
    OPENRAVE_ASSERT_OP((int)weights.size(),==,_statedof);
    Reset();
    _weights = weights;
    _numnodes = 0;
    _base = 2; // optimal is 1.3?
    _fBaseInv = 1/_base;
    _fBaseInv2 = 1/Sqr(_base);
    _fBaseChildMult = 1/(_base-1);
    _maxdistance = maxdistance;
    _maxlevel = ceilf(RaveLog(_maxdistance)/RaveLog(_base));
    _minlevel = _maxlevel - 1;
    _fMaxLevelBound = RavePow(_base, _maxlevel);
    int enclevel = _EncodeLevel(_maxlevel);
    if( enclevel >= (int)_vsetLevelNodes.size() ) {
        _vsetLevelNodes.resize(enclevel+1);
    }
}

void CacheTree::Reset()
{
    // make sure all children are deleted
    for(size_t ilevel = 0; ilevel < _vsetLevelNodes.size(); ++ilevel) {
        FOREACH(itnode, _vsetLevelNodes[ilevel]) {
            (*itnode)->~CacheTreeNode();
        }
    }
    FOREACH(itchildren, _vsetLevelNodes) {
        itchildren->clear();
    }
    _poolNodes.purge_memory();
    _numnodes = 0;
}

#ifdef _DEBUG
static int s_CacheTreeId = 0;
#endif

CacheTreeNodePtr CacheTree::_CreateCacheTreeNode(const std::vector<dReal>& cs, CollisionReportPtr report)
{
    // allocate memory for the structur and the internal state vectors
    void* pmemory = _poolNodes.malloc();
    //Vector* plinkspheres = (Vector*)((uint8_t*)pmemory + sizeof(CacheTreeNode) + sizeof(dReal)*_statedof);
    CacheTreeNodePtr newnode = new (pmemory) CacheTreeNode(cs, NULL);
#ifdef _DEBUG
    newnode->id = s_CacheTreeId++;
#endif
    newnode->SetCollisionInfo(report);
    return newnode;
}

CacheTreeNodePtr CacheTree::_CloneCacheTreeNode(CacheTreeNodeConstPtr refnode)
{
    // allocate memory for the structur and the internal state vectors
    void* pmemory = _poolNodes.malloc();
    //Vector* plinkspheres = (Vector*)((uint8_t*)pmemory + sizeof(CacheTreeNode) + sizeof(dReal)*_statedof);
    CacheTreeNodePtr clonenode = new (pmemory) CacheTreeNode(refnode->GetConfigurationState(), _statedof, refnode->_plinkspheres);
#ifdef _DEBUG
    clonenode->id = s_CacheTreeId++;
#endif
    clonenode->_conftype = refnode->_conftype;
    if( clonenode->IsInCollision() ) {
        clonenode->_collidinglink = refnode->_collidinglink;
        clonenode->_collidinglinktrans = refnode->_collidinglinktrans;
        clonenode->_robotlinkindex = refnode->_robotlinkindex;
    }
    return clonenode;
}

void CacheTree::_DeleteCacheTreeNode(CacheTreeNodePtr pnode)
{
    pnode->~CacheTreeNode();
    _poolNodes.free(pnode);
}

//dReal CacheTree::ComputeDistance(CacheTreeNodePtr vi, CacheTreeNodePtr vf)
//{
//    dReal distance = _ComputeDistance(vi->GetConfigurationState(), vf->GetConfigurationState());
//
//    // use this distance information to update the upper bounds stored on the node
//    vi->UpdateApproximates(distance,vf);
//    vf->UpdateApproximates(distance,vi);
//
//    return distance;
//}

dReal CacheTree::ComputeDistance(const std::vector<dReal>& cstatei, const std::vector<dReal>& cstatef) const
{
    return _ComputeDistance(&cstatei[0], &cstatef[0]);
}

dReal CacheTree::_ComputeDistance(const dReal* cstatei, const dReal* cstatef) const
{
    dReal distance = 0;
    for (size_t i = 0; i < _weights.size(); ++i) {
        dReal f = (cstatei[i] - cstatef[i]) * _weights[i];
        distance += f*f;
    }
    return distance;
}

void CacheTree::SetWeights(const std::vector<dReal>& weights)
{
    OPENRAVE_ASSERT_OP((int)weights.size(),==,_statedof);
    Reset();
    _weights = weights;
}

void CacheTree::SetMaxDistance(dReal maxdistance)
{
    Reset();
    _maxdistance = maxdistance;
    // have to update the max level?
}

std::pair<CacheTreeNodeConstPtr, dReal> CacheTree::FindNearestNode(const std::vector<dReal>& vquerystate, dReal distancebound, ConfigurationNodeType conftype) const
{
    if( _numnodes == 0 ) {
        return make_pair(CacheTreeNodeConstPtr(), dReal(0));
    }

    CacheTreeNodeConstPtr pbestnode=NULL;
    dReal bestdist = std::numeric_limits<dReal>::infinity();
    OPENRAVE_ASSERT_OP(vquerystate.size(),==,_weights.size());
    const dReal* pquerystate = &vquerystate[0];

    int currentlevel = _maxlevel; // where the root node is
    // traverse all levels gathering up the children at each level
    dReal fLevelBound = _fMaxLevelBound;
    _vCurrentLevelNodes.resize(1);
    _vCurrentLevelNodes[0].first = *_vsetLevelNodes.at(_EncodeLevel(_maxlevel)).begin();
    _vCurrentLevelNodes[0].second = _ComputeDistance(pquerystate, _vCurrentLevelNodes[0].first->GetConfigurationState());
    if( (conftype == CNT_Any || _vCurrentLevelNodes[0].first->GetType() == conftype) && _vCurrentLevelNodes[0].first->_usenn ) {
        pbestnode = _vCurrentLevelNodes[0].first;
        bestdist = _vCurrentLevelNodes[0].second;
    }
    while(_vCurrentLevelNodes.size() > 0 ) {
        _vNextLevelNodes.resize(0);
        //RAVELOG_VERBOSE_FORMAT("level %d (%f) has %d nodes", currentlevel%fLevelBound%_vCurrentLevelNodes.size());
        dReal minchilddist=std::numeric_limits<dReal>::infinity();
        FOREACH(itcurrentnode, _vCurrentLevelNodes) {
            // only take the children whose distances are within the bound
            FOREACHC(itchild, itcurrentnode->first->_vchildren) {
                dReal curdist = _ComputeDistance(pquerystate, (*itchild)->GetConfigurationState());
                if( curdist < bestdist ) {
                    if( (*itchild)->_usenn && (conftype == CNT_Any || (*itchild)->GetType() == conftype) ) {
                        bestdist = curdist;
                        pbestnode = *itchild;
                        if( distancebound > 0 && bestdist <= distancebound ) {
                            return make_pair(pbestnode, bestdist);
                        }
                    }
                }
                _vNextLevelNodes.push_back(make_pair(*itchild, curdist));
                if( minchilddist > curdist ) {
                    minchilddist = RaveSqrt(curdist);
                }
            }
        }

        _vCurrentLevelNodes.resize(0);
        dReal ftestbound = Sqr(minchilddist + fLevelBound);
        FOREACH(itnode, _vNextLevelNodes) {
            if( itnode->second < ftestbound ) {
                _vCurrentLevelNodes.push_back(*itnode);
            }
        }
        currentlevel -= 1;
        fLevelBound *= _fBaseInv;
    }
    //RAVELOG_VERBOSE_FORMAT("query went through %d levels", (_maxlevel-currentlevel));
    if( !!pbestnode && (distancebound <= 0 || bestdist <= distancebound) ) {
        return make_pair(pbestnode, RaveSqrt(bestdist));
    }
    // failed radius search, so should return empty
    return make_pair(CacheTreeNodeConstPtr(), dReal(0));
}

std::pair<CacheTreeNodeConstPtr, dReal> CacheTree::FindNearestNode(const std::vector<dReal>& vquerystate, dReal collisionthresh, dReal freespacethresh) const
{
    std::pair<CacheTreeNodeConstPtr, dReal> bestnode;
    bestnode.first = NULL;
    bestnode.second = std::numeric_limits<dReal>::infinity();
    if( _numnodes == 0 ) {
        return bestnode;
    }

    OPENRAVE_ASSERT_OP(vquerystate.size(),==,_weights.size());
    // first localmax is distance from this node to the root
    const dReal* pquerystate = &vquerystate[0];

    dReal collisionthresh2 = Sqr(collisionthresh), freespacethresh2 = Sqr(freespacethresh);
    // traverse all levels gathering up the children at each level
    int currentlevel = _maxlevel; // where the root node is
    dReal fLevelBound = _fMaxLevelBound;
    {
        CacheTreeNodePtr proot = *_vsetLevelNodes.at(_EncodeLevel(_maxlevel)).begin();
        dReal curdist = _ComputeDistance(pquerystate, proot->GetConfigurationState());
        if( proot->_usenn ) {
            ConfigurationNodeType cntype = proot->GetType();
            if( cntype == CNT_Collision && curdist <= collisionthresh2 ) {
                return make_pair(proot,curdist);
            }
            else if( cntype == CNT_Free && curdist <= freespacethresh2 ) {
                // there still could be a node lower in the hierarchy whose collision is closer...
                bestnode = make_pair(proot,curdist);
            }
        }
        _vCurrentLevelNodes.resize(1);
        _vCurrentLevelNodes[0].first = proot;
        _vCurrentLevelNodes[0].second = curdist;
    }
    dReal pruneradius = Sqr(_maxdistance); // the radius to prune all _vCurrentLevelNodes when going through them. Equivalent to min(query,children) + levelbound from the previous iteration
    while(_vCurrentLevelNodes.size() > 0 ) {
        _vNextLevelNodes.resize(0);
        //RAVELOG_VERBOSE_FORMAT("level %d (%f) has %d nodes", currentlevel%fLevelBound%_vCurrentLevelNodes.size());
        dReal minchilddist=_maxdistance;
        FOREACH(itcurrentnode, _vCurrentLevelNodes) {
            if( itcurrentnode->second > pruneradius ) {
                continue;
            }
            dReal comparedist = Sqr(minchilddist + fLevelBound);
            // only take the children whose distances are within the bound
            FOREACHC(itchild, itcurrentnode->first->_vchildren) {
                dReal curdist = _ComputeDistance(pquerystate, (*itchild)->GetConfigurationState());
                if( (*itchild)->_usenn ) {
                    ConfigurationNodeType cntype = (*itchild)->GetType();
                    if( cntype == CNT_Collision && curdist <= collisionthresh2 ) {
                        return make_pair(*itchild, RaveSqrt(curdist));
                    }
                    else if( cntype == CNT_Free && curdist <= freespacethresh2 ) {
                        // there still could be a node lower in the hierarchy whose collision is closer...
                        if( curdist < bestnode.second ) {
                            bestnode = make_pair(*itchild, curdist);
                        }
                    }
                }
                if( curdist < comparedist ) {
                    _vNextLevelNodes.push_back(make_pair(*itchild, curdist));
                    if( minchilddist > curdist ) {
                        minchilddist = RaveSqrt(curdist);
                        comparedist = Sqr(minchilddist + fLevelBound);
                    }
                }
            }
        }

        _vCurrentLevelNodes.swap(_vNextLevelNodes);
        pruneradius = Sqr(minchilddist + fLevelBound);
        currentlevel -= 1;
        fLevelBound *= _fBaseInv;
    }
    // if here, then either found a free node within the bounds, or could not find any nodes
    // failed radius search, so should return empty
    if( !!bestnode.first ) {
        bestnode.second = RaveSqrt(bestnode.second);
    }
    return bestnode;
}

int CacheTree::InsertNode(const std::vector<dReal>& cs, CollisionReportPtr report, dReal fMaxSeparationDist)
{
    OPENRAVE_ASSERT_OP(cs.size(),==,_weights.size());
    CacheTreeNodePtr nodein = _CreateCacheTreeNode(cs, report);
    // if there is no root, make this the root, otherwise call the lowlevel  insert
    if( _numnodes == 0 ) {
        // no root
        _vsetLevelNodes.at(_EncodeLevel(_maxlevel)).insert(nodein); // add to the level
        _numnodes += 1;
        nodein->_level = _maxlevel;
        return 1;
    }

    std::vector<std::pair<CacheTreeNodePtr, dReal> > vchain;
//    if( _numnodes > 100 ) {
//        CacheTreeNodePtr testnode=NULL;
//        FOREACHC(itnode, _vsetLevelNodes.at(_EncodeLevel(2))) {
//            if( (*itnode)->id == 125 ) {
//                testnode = *itnode;
//                break;
//            }
//        }
//        if( !!testnode ) {
//            vchain.insert(vchain.begin(), make_pair(testnode, _ComputeDistance(testnode->GetConfigurationState(), nodein->GetConfigurationState())));
//            for(int currentlevel=3; currentlevel <= _maxlevel; ++currentlevel) {
//                // find its parents
//                int nfound = 0;
//                FOREACH(ittestnode, _vsetLevelNodes.at(_EncodeLevel(currentlevel))) {
//                    if( find((*ittestnode)->_vchildren.begin(), (*ittestnode)->_vchildren.end(), testnode) != (*ittestnode)->_vchildren.end() ) {
//                        nfound++;
//                        testnode = *ittestnode;
//                        vchain.insert(vchain.begin(), make_pair(testnode, _ComputeDistance(testnode->GetConfigurationState(), nodein->GetConfigurationState())));
//                    }
//                }
//                BOOST_ASSERT(nfound==1);
//            }
//        }
//    }

    _vCurrentLevelNodes.resize(1);
    _vCurrentLevelNodes[0].first = *_vsetLevelNodes.at(_EncodeLevel(_maxlevel)).begin();
    _vCurrentLevelNodes[0].second = _ComputeDistance(_vCurrentLevelNodes[0].first->GetConfigurationState(), &cs[0]);
    int nParentFound = _Insert(nodein, _vCurrentLevelNodes, _maxlevel, Sqr(_fMaxLevelBound), Sqr(fMaxSeparationDist));
    if( nParentFound != 1 ) {
        _DeleteCacheTreeNode(nodein);
    }
    return nParentFound;
}

int CacheTree::_Insert(CacheTreeNodePtr nodein, const std::vector< std::pair<CacheTreeNodePtr, dReal> >& vCurrentLevelNodes, int currentlevel, dReal fLevelBound2, dReal fMaxSeparationDist2)
{
#ifdef _DEBUG
    // copy for debugging
    std::vector< std::pair<CacheTreeNodePtr, dReal> > vLocalLevelNodes = vCurrentLevelNodes;
#endif

    dReal closestDist=0;
    CacheTreeNodePtr closestNodeInRange=NULL; /// one of the nodes in vCurrentLevelNodes such that its distance to nodein is <= fLevelBound
    int enclevel = _EncodeLevel(currentlevel);
    dReal fChildLevelBound2 = fLevelBound2*Sqr(_fBaseChildMult);
    if( enclevel < (int)_vsetLevelNodes.size() ) {
        // build the level below
        _vNextLevelNodes.resize(0);
        FOREACHC(itcurrentnode, vCurrentLevelNodes) {
            if( itcurrentnode->second <= fLevelBound2 ) {
                if( !closestNodeInRange ) {
                    closestNodeInRange = itcurrentnode->first;
                    closestDist = itcurrentnode->second;
                }
                else {
                    if(  itcurrentnode->second < closestDist-g_fEpsilonLinear ) {
                        closestNodeInRange = itcurrentnode->first;
                        closestDist = itcurrentnode->second;
                    }
                    // if distances are close, get the node on the lowest level...
                    else if( itcurrentnode->second < closestDist+g_fEpsilonLinear && itcurrentnode->first->_level < closestNodeInRange->_level ) {
                        closestNodeInRange = itcurrentnode->first;
                        closestDist = itcurrentnode->second;
                    }
                }
                if( closestDist < fMaxSeparationDist2 && (!nodein->IsInCollision() || closestNodeInRange->IsInCollision()) ) {
                    // pretty close, so return as if node was added
                    return -1;
                }
            }
            if( itcurrentnode->second <= fChildLevelBound2 ) {
                // node is part of all sets below its level
                _vNextLevelNodes.push_back(*itcurrentnode);
            }
            // only take the children whose distances are within the bound
            if( itcurrentnode->first->_level == currentlevel ) {
                FOREACHC(itchild, itcurrentnode->first->_vchildren) {
                    dReal curdist = _ComputeDistance(nodein->GetConfigurationState(), (*itchild)->GetConfigurationState());
                    if( curdist <= fChildLevelBound2 ) {
                        _vNextLevelNodes.push_back(make_pair(*itchild, curdist));
                    }
                }
            }
        }

        if( _vNextLevelNodes.size() > 0 ) {
            _vCurrentLevelNodes.swap(_vNextLevelNodes); // invalidates vCurrentLevelNodes
            // note that after _Insert call, _vCurrentLevelNodes could be complete lost/reset
            int nParentFound = _Insert(nodein, _vCurrentLevelNodes, currentlevel-1, fLevelBound2*_fBaseInv2, fMaxSeparationDist2);
            if( nParentFound != 0 ) {
                return nParentFound;
            }
        }
    }
    else {
        FOREACHC(itcurrentnode, vCurrentLevelNodes) {
            if( itcurrentnode->second <= fLevelBound2 ) {
                if( !closestNodeInRange ) {
                    closestNodeInRange = itcurrentnode->first;
                    closestDist = itcurrentnode->second;
                }
                else {
                    if(  itcurrentnode->second < closestDist-g_fEpsilonLinear ) {
                        closestNodeInRange = itcurrentnode->first;
                        closestDist = itcurrentnode->second;
                    }
                    // if distances are close, get the node on the lowest level...
                    else if( itcurrentnode->second < closestDist+g_fEpsilonLinear && itcurrentnode->first->_level < closestNodeInRange->_level ) {
                        closestNodeInRange = itcurrentnode->first;
                        closestDist = itcurrentnode->second;
                    }
                }
                if( closestDist < fMaxSeparationDist2 && (!nodein->IsInCollision() || closestNodeInRange->IsInCollision()) ) {
                    // pretty close, so return as if node was added
                    return -1;
                }
            }
        }
    }

    if( !closestNodeInRange ) {
        return 0;
    }

    _InsertDirectly(nodein, closestNodeInRange, closestDist, currentlevel-1, fLevelBound2*_fBaseInv2);
    _numnodes += 1;
    return 1;
}

bool CacheTree::_InsertDirectly(CacheTreeNodePtr nodein, CacheTreeNodePtr parentnode, dReal parentdist, int maxinsertlevel, dReal fInsertLevelBound2)
{
    int insertlevel = maxinsertlevel;
    if( parentdist <= g_fEpsilonLinear ) {
        // pretty close, so notify parent that there's a similar child already underneath it
        if( parentnode->_hasselfchild ) {
            // already has a similar child, so go one level below...?
            FOREACH(itchild, parentnode->_vchildren) {
                dReal childdist = _ComputeDistance(nodein->GetConfigurationState(), (*itchild)->GetConfigurationState());
                if( childdist <= g_fEpsilonLinear ) {
                    return _InsertDirectly(nodein, *itchild, childdist, maxinsertlevel-1, fInsertLevelBound2*_fBaseInv2);
                }
            }
            RAVELOG_WARN("inconsistent node found\n");
            return false;
        }
    }
    else {
        // depending on parentdist, might have to insert at a lower level in order to keep the sibling invariant
        dReal fChildLevelBound2 = fInsertLevelBound2;
        while(parentdist < fChildLevelBound2) {
            fChildLevelBound2 *= _fBaseInv2;
            insertlevel--;
        }
    }

    // have to add at insertlevel. If currentNodeInRange->_level is > insertlevel+1, will have to clone it. note that it will still represent the same RRT node with same rrtparent
    while( parentnode->_level > insertlevel+1 ) {
        CacheTreeNodePtr clonenode = _CloneCacheTreeNode(parentnode);
        clonenode->_level = parentnode->_level-1;
        parentnode->_vchildren.push_back(clonenode);
        parentnode->_hasselfchild = 1;
        int encclonelevel = _EncodeLevel(clonenode->_level);
        if( encclonelevel >= (int)_vsetLevelNodes.size() ) {
            _vsetLevelNodes.resize(encclonelevel+1);
        }
        _vsetLevelNodes.at(encclonelevel).insert(clonenode);
        _numnodes +=1;
        parentnode = clonenode;
    }

    if( parentdist <= g_fEpsilonLinear ) {
        parentnode->_hasselfchild = 1;
    }
    nodein->_level = insertlevel;
    int enclevel2 = _EncodeLevel(nodein->_level);
    if( enclevel2 >= (int)_vsetLevelNodes.size() ) {
        _vsetLevelNodes.resize(enclevel2+1);
    }
    _vsetLevelNodes.at(enclevel2).insert(nodein);
    parentnode->_vchildren.push_back(nodein);

    if( _minlevel > nodein->_level ) {
        _minlevel = nodein->_level;
    }
    return true;
}

bool CacheTree::RemoveNode(CacheTreeNodeConstPtr _removenode)
{
    if( _numnodes == 0 ) {
        return false;
    }

    CacheTreeNodePtr removenode = const_cast<CacheTreeNodePtr>(_removenode);

    CacheTreeNodePtr proot = *_vsetLevelNodes.at(_EncodeLevel(_maxlevel)).begin();
    if( _numnodes == 1 && removenode == proot ) {
        Reset();
        return true;
    }

    if( _maxlevel-_minlevel >= (int)_vvCacheNodes.size() ) {
        _vvCacheNodes.resize(_maxlevel-_minlevel+1);
    }
    FOREACH(it, _vvCacheNodes) {
        it->resize(0);
    }
    _vvCacheNodes.at(0).push_back(proot);
    bool bRemoved = _Remove(removenode, _vvCacheNodes, _maxlevel, Sqr(_fMaxLevelBound));
    if( bRemoved ) {
        _DeleteCacheTreeNode(removenode);
    }
    if( removenode == proot ) {
        BOOST_ASSERT(_vvCacheNodes.at(0).size()==2); // instead of root, another node should have been added
        BOOST_ASSERT(_vsetLevelNodes.at(_EncodeLevel(_maxlevel)).size()==1);
        //_vsetLevelNodes.at(_EncodeLevel(_maxlevel)).clear();
        _vsetLevelNodes.at(_EncodeLevel(_maxlevel)).erase(proot);
        bRemoved = true;
        _numnodes--;
    }

    return bRemoved;
}

bool CacheTree::_Remove(CacheTreeNodePtr removenode, std::vector< std::vector<CacheTreeNodePtr> >& vvCoverSetNodes, int currentlevel, dReal fLevelBound2)
{
    int enclevel = _EncodeLevel(currentlevel);
    if( enclevel >= (int)_vsetLevelNodes.size() ) {
        return false;
    }

    // build the level below
    std::set<CacheTreeNodePtr>& setLevelRawChildren = _vsetLevelNodes.at(enclevel);
    int coverindex = _maxlevel-(currentlevel-1);
    if( coverindex >= (int)vvCoverSetNodes.size() ) {
        vvCoverSetNodes.resize(coverindex+(_maxlevel-_minlevel)+1);
    }
    std::vector<CacheTreeNodePtr>& vNextLevelNodes = vvCoverSetNodes[coverindex];
    vNextLevelNodes.resize(0);

    bool bfound = false;
    FOREACH(itcurrentnode, vvCoverSetNodes.at(coverindex-1)) {
        // only take the children whose distances are within the bound
        if( setLevelRawChildren.find(*itcurrentnode) != setLevelRawChildren.end() ) {
            std::vector<CacheTreeNodePtr>::iterator itchild = (*itcurrentnode)->_vchildren.begin();
            while(itchild != (*itcurrentnode)->_vchildren.end() ) {
                dReal curdist = _ComputeDistance(removenode->GetConfigurationState(), (*itchild)->GetConfigurationState());
                if( *itchild == removenode ) {
                    vNextLevelNodes.resize(0);
                    vNextLevelNodes.push_back(*itchild);
                    itchild = (*itcurrentnode)->_vchildren.erase(itchild);
                    bfound = true;
                }
                else {
                    if( curdist <= fLevelBound2 ) {
                        vNextLevelNodes.push_back(*itchild);
                    }
                    ++itchild;
                }
            }
        }
    }

    bool bRemoved = _Remove(removenode, vvCoverSetNodes, currentlevel-1, fLevelBound2*_fBaseInv2);

    if( !bRemoved && removenode->_level == currentlevel && find(vvCoverSetNodes.at(coverindex-1).begin(), vvCoverSetNodes.at(coverindex-1).end(), removenode) != vvCoverSetNodes.at(coverindex-1).end() ) {
        // for each child, find a more suitable parent
        FOREACH(itchild, removenode->_vchildren) {
            int parentlevel = currentlevel;
            dReal fParentLevelBound2 = fLevelBound2;
            dReal closestdist=0;
            CacheTreeNodePtr closestNode = NULL;
            while(parentlevel <= _maxlevel  ) {
                FOREACHC(itnode, vvCoverSetNodes.at(_maxlevel-parentlevel)) {
                    if( *itnode == removenode ) {
                        continue;
                    }
                    dReal curdist = _ComputeDistance((*itchild)->GetConfigurationState(), (*itnode)->GetConfigurationState());
                    if( curdist < fParentLevelBound2 ) {
                        if( !closestNode || curdist < closestdist ) {
                            closestdist = curdist;
                            closestNode = *itnode;
                        }
                    }
                }
                if( !!closestNode ) {
                    CacheTreeNodePtr nodechild = *itchild;
                    while( nodechild->_level < closestNode->_level-1 ) {
                        CacheTreeNodePtr clonenode = _CloneCacheTreeNode(nodechild);
                        clonenode->_level = nodechild->_level+1;
                        clonenode->_vchildren.push_back(nodechild);
                        clonenode->_hasselfchild = 1;
                        int encclonelevel = _EncodeLevel(clonenode->_level);
                        if( encclonelevel >= (int)_vsetLevelNodes.size() ) {
                            _vsetLevelNodes.resize(encclonelevel+1);
                        }
                        _vsetLevelNodes.at(encclonelevel).insert(clonenode);
                        _numnodes +=1;
                        vvCoverSetNodes.at(_maxlevel-clonenode->_level).push_back(clonenode);
                        nodechild = clonenode;
                    }

                    if( closestdist <= g_fEpsilonLinear ) {
                        closestNode->_hasselfchild = 1;
                    }

                    //_vsetLevelNodes.at(enclevel2).insert(nodechild);
                    closestNode->_vchildren.push_back(nodechild);

                    // closest node was found in parentlevel, so add to the children
                    break;
                }

                // try a higher level
                parentlevel += 1;
                fParentLevelBound2 *= Sqr(_base);
            }
            if( !closestNode ) {
                BOOST_ASSERT(parentlevel>_maxlevel);
                // occurs when root node is being removed and new children have no where to go?
                _vsetLevelNodes.at(_EncodeLevel(_maxlevel)).insert(*itchild);
                vvCoverSetNodes.at(0).push_back(*itchild);
            }
        }
        // remove the node
        size_t erased = setLevelRawChildren.erase(removenode);
        BOOST_ASSERT(erased==1);
        bRemoved = true;
        _numnodes--;
    }
    return bRemoved;
}

void CacheTree::UpdateTree()
{

}

bool CacheTree::Validate()
{
    if( _numnodes == 0 ) {
        return _numnodes==0;
    }

    if( _vsetLevelNodes.at(_EncodeLevel(_maxlevel)).size() != 1 ) {
        RAVELOG_WARN("more than 1 root node\n");
        return false;
    }

    dReal fLevelBound = _fMaxLevelBound;
    std::vector<CacheTreeNodePtr> vAccumNodes; vAccumNodes.reserve(_numnodes);
    std::map<CacheTreeNodePtr, CacheTreeNodePtr> mapNodeParents;
    size_t nallchildren = 0;
    size_t numnodes = 0;
    for(int currentlevel = _maxlevel; currentlevel >= _minlevel; --currentlevel, fLevelBound *= _fBaseInv ) {
        int enclevel = _EncodeLevel(currentlevel);
        if( enclevel >= (int)_vsetLevelNodes.size() ) {
            continue;
        }

        const std::set<CacheTreeNodePtr>& setLevelRawChildren = _vsetLevelNodes.at(enclevel);
        FOREACHC(itnode, setLevelRawChildren) {
            FOREACH(itchild, (*itnode)->_vchildren) {
                dReal curdist = RaveSqrt(_ComputeDistance((*itnode)->GetConfigurationState(), (*itchild)->GetConfigurationState()));
                if( curdist > fLevelBound+g_fEpsilonLinear ) {
#ifdef _DEBUG
                    RAVELOG_WARN_FORMAT("invalid parent child nodes %d, %d at level %d (%f), dist=%f", (*itnode)->id%(*itchild)->id%currentlevel%fLevelBound%curdist);
#else
                    RAVELOG_WARN_FORMAT("invalid parent child nodes at level %d (%f), dist=%f", currentlevel%fLevelBound%curdist);
#endif
                    return false;
                }
            }
            nallchildren += (*itnode)->_vchildren.size();
            if( !(*itnode)->_hasselfchild ) {
                vAccumNodes.push_back(*itnode);
            }

            if( currentlevel < _maxlevel ) {
                // find its parents
                int nfound = 0;
                FOREACH(ittestnode, _vsetLevelNodes.at(_EncodeLevel(currentlevel+1))) {
                    if( find((*ittestnode)->_vchildren.begin(), (*ittestnode)->_vchildren.end(), *itnode) != (*ittestnode)->_vchildren.end() ) {
                        ++nfound;
                        mapNodeParents[*itnode] = *ittestnode;
                    }
                }
                BOOST_ASSERT(nfound==1);

                // check that the child is not that far away from the parent
                CacheTreeNodePtr nodeparent = mapNodeParents[*itnode];
                while(!!nodeparent) {
                    dReal dist = RaveSqrt(_ComputeDistance(nodeparent->GetConfigurationState(), (*itnode)->GetConfigurationState()));
                    if( dist > RavePow(_base,nodeparent->_level+1)*_fBaseChildMult ) {
#ifdef _DEBUG
                        RAVELOG_WARN_FORMAT("node %d (level=%d) has parent %d (level=%d) such that dist %f > %f", (*itnode)->id%(*itnode)->_level%nodeparent->id%nodeparent->_level%dist%(RavePow(_base,nodeparent->_level+1)*_fBaseChildMult));
#else
                        RAVELOG_WARN_FORMAT("node (level=%d) has parent (level=%d) such that dist %f > %f", (*itnode)->_level%nodeparent->_level%dist%(RavePow(_base,nodeparent->_level+1)*_fBaseChildMult));
#endif
                        return false;
                    }
                    nodeparent = mapNodeParents[nodeparent];
                }
            }
        }
        numnodes += setLevelRawChildren.size();

        for(size_t i = 0; i < vAccumNodes.size(); ++i) {
            for(size_t j = i+1; j < vAccumNodes.size(); ++j) {
                dReal curdist = RaveSqrt(_ComputeDistance(vAccumNodes[i]->GetConfigurationState(), vAccumNodes[j]->GetConfigurationState()));
                if( curdist <= fLevelBound ) {
#ifdef _DEBUG
                    RAVELOG_WARN_FORMAT("invalid sibling nodes %d, %d  at level %d (%f), dist=%f", vAccumNodes[i]->id%vAccumNodes[j]->id%currentlevel%fLevelBound%curdist);
#else
                    RAVELOG_WARN_FORMAT("invalid sibling nodes %d, %d  at level %d (%f), dist=%f", i%j%currentlevel%fLevelBound%curdist);
#endif
                    return false;
                }
            }
        }
    }

    if( _numnodes != (int)numnodes ) {
        RAVELOG_WARN_FORMAT("num predicted nodes (%d) does not match computed nodes (%d)", _numnodes%numnodes);
        return false;
    }
    if( _numnodes != (int)nallchildren+1 ) {
        RAVELOG_WARN_FORMAT("num predicted nodes (%d) does not match computed nodes from children (%d)", _numnodes%(nallchildren+1));
        return false;
    }

    return true;
}

ConfigurationCache::ConfigurationCache(RobotBasePtr pstaterobot) : _cachetree(pstaterobot->GetActiveDOF())
{
    _userdatakey = std::string("configurationcache") + boost::lexical_cast<std::string>(this);
    _qtime = 0;
    _itime = 0;
    _profile = false;
    _pstaterobot = pstaterobot;
    _penv = pstaterobot->GetEnv();
    _handleBodyAddRemove = _penv->RegisterBodyCallback(boost::bind(&ConfigurationCache::_UpdateAddRemoveBodies, this, _1, _2));

    std::vector<KinBodyPtr> vGrabbedBodies;
    _pstaterobot->GetGrabbed(vGrabbedBodies);
    _setGrabbedBodies.insert(vGrabbedBodies.begin(), vGrabbedBodies.end());
    std::vector<KinBodyPtr> vnewenvbodies;
    _penv->GetBodies(vnewenvbodies);
    FOREACHC(itbody, vnewenvbodies) {
        if( *itbody != pstaterobot && !pstaterobot->IsGrabbing(*itbody) ) {
            KinBodyCachedDataPtr pinfo(new KinBodyCachedData());
            pinfo->_changehandle = (*itbody)->RegisterChangeCallback(KinBody::Prop_LinkGeometry|KinBody::Prop_LinkEnable|KinBody::Prop_LinkTransforms, boost::bind(&ConfigurationCache::_UpdateUntrackedBody, this, *itbody));
            (*itbody)->SetUserData(_userdatakey, pinfo);
        }
    }

    _vRobotActiveIndices = pstaterobot->GetActiveDOFIndices();
    _nRobotAffineDOF = pstaterobot->GetAffineDOF();
    _vRobotRotationAxis = pstaterobot->GetAffineRotationAxis();

    std::vector<dReal> vweights;
    pstaterobot->GetActiveDOFResolutions(vweights);

    // if weights are zero, used a default value
    FOREACH(itweight, vweights) {
        if( *itweight > 0 ) {
            *itweight = 1 / *itweight;
        }
        else {
            *itweight = 100;
        }
    }

    // set default values for collisionthresh and insertiondistance
    _collisionthresh = 1.0; //1.0; //minres; //smallest resolution
    _freespacethresh = 0.2;
    _insertiondistancemult = 0.5; //0.5; //0.02;  //???

    _pstaterobot->GetActiveDOFLimits(_lowerlimit, _upperlimit);

    _jointchangehandle = pstaterobot->RegisterChangeCallback(KinBody::Prop_JointLimits, boost::bind(&ConfigurationCache::_UpdateRobotJointLimits, this));
    _jointchangehandle = pstaterobot->RegisterChangeCallback(KinBody::Prop_RobotGrabbed, boost::bind(&ConfigurationCache::_UpdateRobotGrabbed, this));

    // using L1, get the maximumdistance
    // distance has to be computed in the same way as CacheTreeNode.GetDistance()
    // otherwise, distances larger than this value could be inserted into the tree
    dReal maxdistance = 0;
    for (size_t i = 0; i < _lowerlimit.size(); ++i) {
        dReal f = (_upperlimit[i] - _lowerlimit[i]) * vweights[i];
        maxdistance += f*f;
    }

    _cachetree.Init(vweights, RaveSqrt(maxdistance));

    if (IS_DEBUGLEVEL(Level_Debug)) {
        stringstream ss; ss << std::setprecision(std::numeric_limits<OpenRAVE::dReal>::digits10+1);
        ss << "Initializing cache,  maxdistance " << _cachetree.GetMaxDistance() << ", collisionthresh " << _collisionthresh << ", _insertiondistancemult "<< _insertiondistancemult << ", weights [";
        for (size_t i = 0; i < vweights.size(); ++i) {
            ss << vweights[i] << " ";
        }
        ss << "]\nupperlimit [";

        for (size_t i = 0; i < _upperlimit.size(); ++i) {
            ss << _upperlimit[i] << " ";
        }

        ss << "]\nlowerlimit [";

        for (size_t i = 0; i < _lowerlimit.size(); ++i) {
            ss << _lowerlimit[i] << " ";
        }
        ss << "]\n";
        RAVELOG_DEBUG(ss.str());
    }
}

void ConfigurationCache::SetWeights(const std::vector<dReal>& weights)
{
    _cachetree.SetWeights(weights);
}

bool ConfigurationCache::InsertConfiguration(const std::vector<dReal>& conf, CollisionReportPtr report, dReal distin)
{
    if( !!report ) {
        if( !!report->plink2 && report->plink2->GetParent() == _pstaterobot ) {
            std::swap(report->plink1, report->plink2);
        }
    }
    int ret = _cachetree.InsertNode(conf, report, !report ? _freespacethresh*_insertiondistancemult : _collisionthresh*_insertiondistancemult);
    BOOST_ASSERT(ret!=0);
    return ret==1;
}

int ConfigurationCache::RemoveConfigurations(const std::vector<dReal>& cs, dReal radius)
{
    // slow implementation for now
    int nremoved=0;
    while(1) {
        std::pair<CacheTreeNodeConstPtr, dReal> neigh = _cachetree.FindNearestNode(cs, radius, CNT_Any);
        if( !neigh.first ) {
            break;
        }
        OPENRAVE_ASSERT_OP(neigh.second,<=,radius);
        bool bremoved = _cachetree.RemoveNode(neigh.first);
        BOOST_ASSERT(bremoved);
        nremoved += 1;
    }
    return nremoved;
}

void ConfigurationCache::GetDOFValues(std::vector<dReal>& values)
{
    // try to get the values without setting state
    _pstaterobot->GetDOFValues(values, _vRobotActiveIndices);
    values.resize(_lowerlimit.size());
    if( _nRobotAffineDOF != 0 ) {
        RaveGetAffineDOFValuesFromTransform(values.begin()+_vRobotActiveIndices.size(), _pstaterobot->GetTransform(), _nRobotAffineDOF, _vRobotRotationAxis);
    }
}

int ConfigurationCache::CheckCollision(const std::vector<dReal>& conf, KinBody::LinkConstPtr& robotlink, KinBody::LinkConstPtr& collidinglink, dReal& closestdist)
{
    std::pair<CacheTreeNodeConstPtr, dReal> knn = _cachetree.FindNearestNode(conf, _collisionthresh, _freespacethresh);
    if( !!knn.first ) {
        closestdist = knn.second;
        if( knn.first->IsInCollision()) {
            robotlink = _pstaterobot->GetLinks().at(knn.first->GetRobotLinkIndex());
            collidinglink = knn.first->GetCollidingLink();
            return 1;
        }
        return 0;
    }
    return -1;
}

int ConfigurationCache::CheckCollision(KinBody::LinkConstPtr& robotlink, KinBody::LinkConstPtr& collidinglink, dReal& closestdist)
{
    std::vector<dReal> conf;
    GetDOFValues(conf);
    return CheckCollision(conf, robotlink, collidinglink, closestdist);
}

void ConfigurationCache::Reset()
{
    _cachetree.Reset();
}

bool ConfigurationCache::Validate()
{
    return _cachetree.Validate();
}

void ConfigurationCache::_UpdateUntrackedBody(KinBodyPtr pbody)
{
    // body's state has changed, so remove collision space and invalidate free space.
    _cachetree.Reset();
}

void ConfigurationCache::_UpdateAddRemoveBodies(KinBodyPtr pbody, int action)
{
    if( action == 1 ) {
        KinBodyCachedDataPtr pinfo(new KinBodyCachedData());
        pinfo->_changehandle = pbody->RegisterChangeCallback(KinBody::Prop_LinkGeometry|KinBody::Prop_LinkEnable|KinBody::Prop_LinkTransforms, boost::bind(&ConfigurationCache::_UpdateUntrackedBody, this, pbody));
        pbody->SetUserData(_userdatakey, pinfo);

        // TODO invalide the freespace of a cache given a new body in the scene
    }
    else if( action == 0 ) {
        pbody->RemoveUserData(_userdatakey);
        // TODO invalidate the collision space of a cache given a body has been removed
    }
}

void ConfigurationCache::_UpdateRobotJointLimits()
{
    _pstaterobot->SetActiveDOFs(_vRobotActiveIndices, _nRobotAffineDOF);
    _pstaterobot->GetActiveDOFLimits(_lowerlimit, _upperlimit);

    // compute new max distance for cache
    // using L1, get the maximumdistance
    // distance has to be computed in the same way as CacheTreeNode.GetDistance()
    // otherwise, distances larger than this value could be inserted into the tree
    dReal maxdistance = 0;
    for (size_t i = 0; i < _lowerlimit.size(); ++i) {
        dReal f = (_upperlimit[i] - _lowerlimit[i]) * _cachetree.GetWeights().at(i);
        maxdistance += f*f;
    }
    maxdistance = RaveSqrt(maxdistance);
    if( maxdistance > _cachetree.GetMaxDistance()+g_fEpsilonLinear ) {
        _cachetree.SetMaxDistance(maxdistance);
    }
}

void ConfigurationCache::_UpdateRobotGrabbed()
{
    _cachetree.Reset();
}

}