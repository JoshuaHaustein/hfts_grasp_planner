#! /usr/bin/python

import HFTSMotion.orRobot.orUtils as hfts_utils
import numpy as np
from math import exp
import openravepy as orpy
import transformations
from scipy.optimize import fmin_cobyla
# from HFTSMotion.orRobot.constraints import *
from RobotiqLoader import RobotiqHand
import sys, time, logging, copy
import itertools, random
from HFTSMotion.orRobot.handBase import InvalidTriangleException
from sets import Set
from utils import objectFileIO

class graspSampler:

    def __init__(self, verbose=False, numHops=2, vis=True):

        self._verbose = verbose
        self._samplerViewer = vis
        self._orEnv = orpy.Environment() # create openrave environment
        self._orEnv.SetDebugLevel(orpy.DebugLevel.Fatal)
        self._orEnv.GetCollisionChecker().SetCollisionOptions(orpy.CollisionOptions.Contacts)
        if vis:
            self._orEnv.SetViewer('qtcoin') # attach viewer (optional)
        self._handLoaded = False

        self._mu = 2.
        self._alpha = 2.
        self._maxIters = 40
        self._hops = numHops
        self._ita = 0.001

        self._graspConf = None
        self._preGraspConf = None
        self._graspPos = None
        self._graspPose = None
        self._graspContacts = None
        self._armConf = None
        self._handPose_lab = None


    def __del__(self):
        orpy.RaveDestroy()


    def loadHand(self, handFile):
        if not self._handLoaded:
            self._robot = RobotiqHand(env = self._orEnv, handFile = handFile)
            self._handMani = self._robot.getHandMani()
            # self._contactN = self._robot.getHandDim()[3]
            self._handLoaded = True
            # shift = transformations.identity_matrix()

            # shift[0,-1] = 0.2
            # self._robot.SetTransform(shift)

        #     if self._verbose:
        #         print 'hand successfully loaded by graspSampler'
        # else:
        #     print "graspSampler::loadHand: hand is already loaded, will not load again unless reload"


# 
#     def loadObj(self, dataPath, objName, nLevel):
# 
#         self._objName = objName
#         self._nLevel = nLevel
#         self._objDataPath = dataPath
#         dataPath = dataPath + '/' + self._objName + '/'
#         objFile = dataPath + '/object.kinbody.xml'
#         oriCOMFile = dataPath + '/COMPy.bin'
#         labelFile = dataPath + 'dataLabeledPy.bin'
# 
#         if not self._objLoaded:
#             self._obj = self._orEnv.ReadKinBodyXMLFile(objFile)
#             self._oriCOM = hfts_utils.readBinaryMatrix(oriCOMFile, 3)
#             self._orEnv.Add(self._obj)
#             objTrans = transformations.identity_matrix()
#             objTrans[0:3, 3] = -self._oriCOM
#             self._obj.SetTransform(objTrans)
#             self._obj.GetLinks()[0].GetGeometries()[0].SetTransparency(0.6)
#             self._dataLabeled = hfts_utils.readBinaryMatrix(labelFile, 6 + self._nLevel)
#             self._levels = hfts_utils.getHFTSLevels(self._dataLabeled[:, 6:])
#             self._objLoaded = True
#             if self._verbose:
#                 print 'object successfully loaded by graspSampler'
#         else:
#             print "graspSampler::loadObj: object is already loaded, will not load again unless reload"
# 
# 
# 
#     def initPlanner(self):
#         if self._verbose:
#             print 'initializing planner'
#         topNodes = self._levels[0] + 1
#         contactLabel = []
# 
#         for i in range(self._contactN):
#             contactLabel.append([random.choice(range(topNodes))])
# 
#         return contactLabel
# 
# 
# 
#     def clearConfigCache(self):
# 
#         self._graspConf = None
#         self._preGraspConf = None
#         self._graspPos = None
#         self._graspPose = None
#         self._graspContacts = None
#         self._armConf = None
#         self._handPose_lab = None
#         shift = transformations.identity_matrix()
#         shift[0,-1] = 0.2
#         self._robot.SetTransform(shift)
#         self._robot.SetDOFValues([0]*7, range(7))
#         #self._orLabEnv.homeRobot()
#         self.handles = []
#         self.tipPNHandler = []
# 
# 
# 
#     def checkGraspIK(self, seed=None, openHandOffset=0.1):
#         # this function can be called after composeGraspInfo is called
#         objPose_lab = self._orLabEnv.getObjTransform(self._objName)
#         objPose = self._obj.GetTransform()
#         handPose_hfts = np.dot(np.linalg.inv(objPose), self._graspPose)
#         handPose_lab = np.dot(objPose_lab, handPose_hfts)
#         bValid, armConf, self._preGraspConf = self._orLabEnv.handCheckIK6D(handPose_lab, self._graspConf,
#                                                                            seed=seed,
#                                                                            openHandOffset=openHandOffset)
# 
#         self._armConf = armConf
#         self._handPose_lab = handPose_lab
#         return bValid
# 
# 
#     def sampleGrasp(self, node, depthLimit, labelCache=None, postOpt=False, openHandOffset=0.1):
# 
#         assert depthLimit >= 0
#         if node.getDepth() >= self._nLevel:
#             raise ValueError('graspSampler::sampleGrasp input node has an invalid depth')
#             
#             
#         if node.getDepth() + depthLimit > self._nLevel:
#             depthLimit = self._nLevel - node.getDepth() # cap
# 
#         seedIk = None
#         if node.getDepth() == 0: # at root
#             contactLabel = self.initPlanner()
#             bestO = -np.inf ## need to also consider non-root nodes
#         else:
#             # If we are not at a leaf node, go down in the hierarchy
#             seedIk = node.getArmConfig()
#             contactLabel = copy.deepcopy(node.getLabels())
#             bestO, contactLabel = self.extendSolution(contactLabel)
# 
#         allowedFingerCombos = None
#         if labelCache is not None:
#             # This currently only works for hops == 2
#             assert self._hops == 2
#             # logging.debug('[GoalSampler::sampleGrasp] Label cache: ' + str(labelCache))
#             allowedFingerCombos = self.computeAllowedContactCombos(node.getDepth(), labelCache)
#             logging.debug('[GoalSampler::sampleGrasp] We have %i allowed contacts' %
#                           len(allowedFingerCombos))
#             if len(allowedFingerCombos) == 0:
#                 logging.warn('[GoalSampler::sampleGrasp] We have no allowed contacts left! Aborting.')
#                 return node
# 
#         self.clearConfigCache()
#         depthLimit -= 1
#         logging.debug('[GoalSampler::sampleGrasp] Sampling a grasp; %i number of iterations' %
#                       self._maxIters)
# 
#         while True:
#             # just do it until depthLimit is reached
#             for iter_now in range(self._maxIters): #/(len(contactLabel[0])*2)):
#                 labels_tmp = self.getSiblingLabels(currLabels=contactLabel,
#                                                    allowedFingerCombos=allowedFingerCombos)
#                 # logging.debug('[GoalSampler::sampleGrasp] Sampled labels are: ' + str(labels_tmp))
#                 s_tmp, r_tmp, o_tmp = self.evaluateGrasp(labels_tmp)
# 
#                 if self.shcEvaluation(o_tmp, bestO):
#                     contactLabel = labels_tmp
#                     bestO = o_tmp
#                     # if self._verbose:
#                         # print '---------------------------------------------------'
#                         # print 'improved at level: %d, iter: %d' % (depthLimit, iter_now)
#                         # print s_tmp, r_tmp, o_tmp
#                         # print '---------------------------------------------------'
# 
# 
#             # extending to next level
#             if depthLimit > 0:
#                 bestO, contactLabel = self.extendSolution(contactLabel)
#                 depthLimit -= 1
# 
#             else: # consider output
#                 self.composeGraspInfo(contactLabel)
#                 if postOpt:
#                     logging.debug('[GraspGoalSampler::sampleGrasp] Doing post optimization for node ' + \
#                                   str(contactLabel))
#                 try:
#                     sampleQ, stability = self.executeInOR(postOpt=postOpt)
#                 except InvalidTriangleException:
#                     self._graspConf = None
#                     sampleQ = 4
#                     stability = 0.0
# 
#                 isLeaf = len(contactLabel[0]) == self._nLevel
#                 isGoalSample = sampleQ == 0 and isLeaf
#                 if not isGoalSample and self._graspConf is not None:
#                     logging.debug('[GraspGoalSampler::sampleGrasp] Approximate has final quality: ' +
#                                   str(sampleQ))
#                     self.avoidCollisionAtFingers(nStep = 20)
#                     openHandOffset = 0.0
#                 logging.debug('[GraspSampler] We sampled a grasp on level ' + str(len(contactLabel[0])))
#                 if isGoalSample:
#                     logging.debug('[GraspSampler] We sampled a goal grasp (might be in collision)!')
#                 if isLeaf:
#                     logging.debug('[GraspSampler] We sampled a leaf')
# 
#                 if self._graspConf is not None:
#                     goodInLabEnv = self.checkGraspIK(seed=seedIk, openHandOffset=openHandOffset)
#                 else:
#                     goodInLabEnv = False
#                 if self._verbose:
#                     print '!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!'
#                     print 'goodInlabEnv: %s, isGoalSample: %s' % (goodInLabEnv, isGoalSample)
#                     print '!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!'
# 
#                 depth = len(contactLabel[0])
#                 possibleNumChildren, possibleNumLeaves = self.getBranchInformation(depth)
#                 return HFTSNode(labels=contactLabel, handConf=self._graspConf,
#                                 preGraspHandConfig=self._preGraspConf, armConf=self._armConf,
#                                 goal=isGoalSample, leaf=isLeaf, valid=goodInLabEnv,
#                                 possibleNumLeaves=possibleNumLeaves, possibleNumChildren=possibleNumChildren,
#                                 quality=stability)
# 
# 
#     def getBranchInformation(self, depth):
#         if depth < self.getMaximumDepth():
#             possibleNumChildren = pow(self._levels[depth] + 1, self._contactN)
#             possibleNumLeaves = 1
#             for d in range(depth, self.getMaximumDepth()):
#                 possibleNumLeaves *= pow(self._levels[depth] + 1, self._contactN)
#         else:
#             possibleNumChildren = 0
#             possibleNumLeaves = 1
#         return (possibleNumChildren, possibleNumLeaves)
# 
# 
#     def avoidCollisionAtFingers(self, nStep = 4, s = 0.6):
#         if self._orEnv.CheckCollision(self._robot.GetLink('root')):
#             return None
#         if self._orEnv.CheckCollision(self._robot.GetLink('A1_Link')):
#             return None
#         if self._orEnv.CheckCollision(self._robot.GetLink('B1_Link')):
#             return None
#         if self._orEnv.CheckCollision(self._robot.GetLink('C1_Link')):
#             return None
#         currConf = self._robot.GetDOFValues()
#         openConfig = np.array([0, -math.pi/2, 0, -math.pi/2, 0, -math.pi/2, 0]) + np.array([s]*7)
#         openConfig[0] = currConf[0]
#         step = (openConfig - currConf) / nStep
#         A = True
#         B = True
#         C = True
#         for i in range(nStep):
#             if A:
#                 if self._orEnv.CheckCollision(self._robot.GetLink('A2_Link')) \
#                    or self._orEnv.CheckCollision(self._robot.GetLink('A3_Link')):
#                     currConf[1:3] += step[1:3]
#                 else:
#                     A = False
#             if B:
#                 if self._orEnv.CheckCollision(self._robot.GetLink('B2_Link')) \
#                    or self._orEnv.CheckCollision(self._robot.GetLink('B3_Link')):
#                     currConf[5:] += step[5:]
#                 else:
#                     B = False
#             if C:
#                 if self._orEnv.CheckCollision(self._robot.GetLink('C2_Link')) \
#                    or self._orEnv.CheckCollision(self._robot.GetLink('C3_Link')):
#                     currConf[3:5] += step[3:5]
#                 else:
#                     C = False
#             self._robot.SetDOFValues(currConf)
#         if A or B or C:
#             pass
#         else:
#             self._graspConf = self._robot.GetDOFValues()
# 
# 
#     def plotClusters(self, contactLabels):
# 
#         if not self._samplerViewer:
#             return
#         self.cloudPlot = []
#         colors = [np.array((1,0,0)), np.array((1,1,0)), np.array((1,0,0))]
# 
#         for i in range(3):
#             label = contactLabels[i]
# 
#             level = len(label) - 1 # indexed from 0
#             idx = np.where((self._dataLabeled[:, 6:7 + level] == label).all(axis=1))
#             points = [self._dataLabeled[t, 0:3] for t in idx][0]
#             points = np.asarray(points)
#             self.cloudPlot.append(self._orEnv.plot3(points=points, pointsize=0.006, colors=colors[i], drawstyle=1))
# 
# 
# 
# 
# 
#     def executeInOR(self, postOpt):
#         self._robot.SetDOFValues(self._graspConf, range(self._robot.getHandDim()[1]))
# 
#         # initial alignment
#         T = self._robot.HandObjTransform(self._graspPos[:3, :3], self._graspContacts[:, :3])
#         self._robot.SetTransform(T)
#         # raw_input('press')
# 
#         if postOpt: # postOpt
#             rot = transformations.rotation_from_matrix(T)
#             # further optimize hand configuration
#             rotParam = rot[1].tolist() + [rot[0]] + T[:3, -1].tolist()
#             fmin_cobyla(self._robot.allObj, self._robot.GetDOFValues() + rotParam, allConstr, rhobeg = .1,
#                         rhoend=1e-4, args=(self._graspContacts[:, :3], self._graspContacts[:, 3:], self._robot), maxfun=1e8, iprint=0)
# 
#         self.complyEndEffectors()
# 
#         self._graspPose = self._robot.GetTransform()
#         self._graspConf = self._robot.GetDOFValues()
#         # self.drawTipPN()
#         ret, stability = self.finalCheck()
#         return ret, stability
#         # if ret < 3:
#         #     return ret
#         # else:
#         #     self.swapContacts([0,2])
#         # code_tmp = self._handMani.encodeGrasp3(self._graspContacts)
#         # dummy, self._graspConf  = self._handMani.predictHandConf(code_tmp)
#         # self._robot.SetDOFValues(self._graspConf, range(self._robot.getHandDim()[1]))
#         # self._graspPos = self._handMani.getOriTipPN(self._graspConf)
#         #
#         # # initial alignment
#         # T = self._robot.HandObjTransform(self._graspPos[:3, :3], self._graspContacts[:, :3])
#         # self._robot.SetTransform(T)
#         #
#         #
#         # if postOpt: # postOpt
#         #     rot = transformations.rotation_from_matrix(T)
#         #     # further optimize hand configuration
#         #     rotParam = rot[1].tolist() + [rot[0]] + T[:3, -1].tolist()
#         #     fmin_cobyla(self._robot.allObj, self._robot.GetDOFValues() + rotParam, allConstr, rhobeg = .1,
#         #                 rhoend=1e-4, args=(self._graspContacts[:, :3], self._graspContacts[:, 3:], self._robot), maxfun=1e8, iprint=0)
#         #
#         # self.complyEndEffectors()
#         #
#         # self._graspPose = self._robot.GetTransform()
#         # self._graspConf = self._robot.GetDOFValues()
#         # # self.drawTipPN()
#         # return self.finalCheck()
#         #
# 
# 
#     def finalCheck(self):
#         stability = -1.0
#         if self._robot.CheckSelfCollision():
#             return 5, stability
#         if self.checkContacts():
#             return 4, stability
#         if self.checkCollision():
#             return 3, stability
#         stability = self.computeRealStability()
#         if stability < 0.001:
#             # print 'real stability: %f' % self.computeRealStability()
#             return 2, stability
#         if self.computeContactQ() > 60:
#             return 1, stability
# 
#         return 0, stability
# 
#     def checkContacts(self):
#         links = self._robot.GetLinks()
#         for link in self._robot.getEndEffectors():
#             if not self._orEnv.CheckCollision(links[link], self._obj):
#                 return True
#         return False
# 
# 
#     def computeRealStability(self):
# 
#         try:
#             q = hfts_utils.computeGraspQualityNeg(self.getRealContacts(), self._mu)
#             return q
#         except:
#             return -1.
# 
# 
#     def computeContactQ(self):
#         try:
#             rc = self.getRealContacts()
#         except:
#             return np.inf
#         tipPN = self._robot.getTipPN()
#         # ret = self._robot.contactsDiff(-rc[:,3:], tipPN[:,3:])
#         ret = 0
#         for i in range(self._robot.getHandDim()[-1]):
#             ret = max(ret, hfts_utils.vecAngelDiff(rc[i, 3:], -tipPN[i, 3:]))
#         return ret
# 
#     def getRealContacts(self):
#         reportX = orpy.CollisionReport()
#         rContacts = []
# 
#         for eel in self._robot.getEndEffectors():
# 
#             self._orEnv.CheckCollision(self._obj, self._robot.GetLinks()[eel],report=reportX)
#             if len(reportX.contacts) == 0:
#                 raise ValueError('no contact found')
#             rContacts.append(np.concatenate((reportX.contacts[0].pos, reportX.contacts[0].norm)))
# 
#         rContacts = np.asarray(rContacts)
#         return rContacts
# 
# 
#     def checkCollision(self):
#         i = -1
# 
#         for link in self._robot.GetLinks():
#             i += 1
# 
#             if i in self._robot.getEndEffectors():
#                 continue
#             if self._orEnv.CheckCollision(link, self._obj):
#                 return True
#         return False
# 
# 
#     def complyEndEffectors(self):
# 
#         curr = self._robot.GetDOFValues()
#         for j in self._robot.getEndJoints():
#             curr[j] = -math.pi/2.
#             # curr[j-1] -=math.pi/36
# 
# 
#         limitL, limitU = self._robot.GetDOFLimits()
#         self._robot.SetDOFValues(np.asarray(curr), range(self._robot.getHandDim()[1]))
# 
#         stepLen = 0.2
# 
#         joints = self._robot.getEndJoints()
#         links = self._robot.getEndEffectors()
# 
# 
# 
#         for i in range(len(joints)):
# 
#             maxStep = 400
#             while self._orEnv.CheckCollision(self._robot.GetLinks()[links[i]-1]):
#                 curr[joints[i]-1] -= stepLen
#                 if curr[joints[i]-1] < limitL[joints[i]-1]:
#                     break
# 
#                 self._robot.SetDOFValues(curr, range(self._robot.getHandDim()[1]))
# 
#         stepLen = 0.01
#         maxStep = 400
#         done = [False] * len(joints)
# 
# 
#         while False in done and maxStep >= 0:
#             curr = self._robot.GetDOFValues()
#             for i in range(len(joints)):
#                 if curr[joints[i]] >= limitU[joints[i]]:
#                     done[i] = True
#                 if not done[i]:
#                     curr[joints[i]] += stepLen
#                     if self._orEnv.CheckCollision(self._robot.GetLinks()[links[i]]):
#                         done[i] = True
#                         curr[joints[i]] += stepLen
# 
#             maxStep -= 1
# 
#             self._robot.SetDOFValues(curr, range(self._robot.getHandDim()[1]))
# 
#     def plotContacts(self, cPoints, clear=False):
#         if not self._samplerViewer:
#             return
#         pointSize = 0.008
#         if clear:
#             self.handles = []
#         colors = [np.array((1,0,0)), np.array((1,1,0)), np.array((1,0,0))]
#         c0 = cPoints[0, :3]
#         c1 = cPoints[1, :3]
#         c2 = cPoints[2, :3]
# 
#         n0 = cPoints[0, 3:]
#         n1 = cPoints[1, 3:]
#         n2 = cPoints[2, 3:]
# 
# 
#         self.handles.append(self._orEnv.plot3(points=c0, pointsize=pointSize, colors=colors[0],drawstyle=1))
#         self.handles.append(self._orEnv.plot3(points=c1, pointsize=pointSize, colors=colors[1],drawstyle=1))
#         self.handles.append(self._orEnv.plot3(points=c2, pointsize=pointSize, colors=colors[2],drawstyle=1))
# 
#         self.handles.append(self._orEnv.drawarrow(p1=c0, p2=c0 + 0.02 * n0,linewidth=0.001,color=colors[0]))
#         self.handles.append(self._orEnv.drawarrow(p1=c1, p2=c1 + 0.02 * n1,linewidth=0.001,color=colors[1]))
#         self.handles.append(self._orEnv.drawarrow(p1=c2, p2=c2 + 0.02 * n2,linewidth=0.001,color=colors[2]))
# 
# 
# 
# 
#     def composeGraspInfo(self, contactLabels):
# 
#         contacts = [] # a list of contact positions and normals
#         for i in range(self._contactN):
#             p, n = self.clusterRep(contactLabels[i])
#             contacts.append(list(p) + list(n))
# 
#         self._graspContacts= np.asarray(contacts)
# 
#         code_tmp = self._handMani.encodeGrasp3(self._graspContacts)
#         dummy, self._graspConf  = self._handMani.predictHandConf(code_tmp)
#         self._graspPos = self._handMani.getOriTipPN(self._graspConf)
# 
# 
# 
#     def extendSolution(self, oldLabels):
#         for label in oldLabels:
#             label.append(np.random.randint(self._levels[len(label)] + 1))
#         s_tmp, r_tmp, o_tmp = self.evaluateGrasp(oldLabels)
# 
#         return o_tmp, oldLabels
# 
#     def clusterRep(self, label):
#         level = len(label) - 1 # indexed from 0
# 
#         idx = np.where((self._dataLabeled[:, 6:7 + level] == label).all(axis=1))
#         points = [self._dataLabeled[t, 0:3] for t in idx][0]
#         normals = [self._dataLabeled[t, 3:6] for t in idx][0]
#         pos = np.sum(points, axis=0) / len(idx[0])
#         normal = np.sum(normals, axis=0) / len(idx[0])
#         normal /= np.linalg.norm(normal)
#         return pos, -normal
# 
#     def swapContacts(self, rows):
#         frm = rows[0]
#         to = rows[1]
#         self._graspContacts[[frm, to],:] = self._graspContacts[[to, frm],:]
# 
#     def evaluateGrasp(self, contactLabel):
# 
#         contacts = [] # a list of contact positions and normals
# 
#         for i in range(self._contactN):
#             p, n = self.clusterRep(contactLabel[i])
#             contacts.append(list(p) + list(n))
# 
#         contacts = np.asarray(contacts)
# 
#         s_tmp = hfts_utils.computeGraspQualityNeg(contacts, self._mu)
#         code_tmp = self._handMani.encodeGrasp3(contacts)
#         r_tmp, dummy = self._handMani.predictHandConf(code_tmp)
#         # o_tmp = s_tmp - self._alpha * r_tmp
#         # TODO: Research topic. This is kind of hack. Another objective function might be better
#         o_tmp = s_tmp / (r_tmp + 0.000001)
#         return s_tmp, r_tmp, o_tmp
# 
# 
# 
#     def shcEvaluation(self, o_tmp, bestO):
#         # if bestO < o_tmp:
#         #     return True
#         # else:
#         #     return False
# 
#         v = (bestO - o_tmp) / self._ita
#         if v < 0: #python overflow
#             return True
#         else:
#             return False
# 
#         p = 1. / (1 + exp(v))
# 
#         return  p > np.random.uniform()
# 
# 
#     def drawTipPN(self):
# 
#         if not self._samplerViewer:
#             return
#         self.tipPNHandler = []
# 
#         tipPN = self._robot.getTipPN()
#         pointSize = 0.008
# 
#         colors = [np.array((1,0,1)), np.array((1,0,1)), np.array((1,0,1))]
#         c0 = tipPN[0, :3]
#         c1 = tipPN[1, :3]
#         c2 = tipPN[2, :3]
# 
#         n0 = tipPN[0, 3:]
#         n1 = tipPN[1, 3:]
#         n2 = tipPN[2, 3:]
# 
# 
#         self.tipPNHandler.append(self._orEnv.plot3(points=c0, pointsize=pointSize, colors=colors[0],drawstyle=1))
#         self.tipPNHandler.append(self._orEnv.plot3(points=c1, pointsize=pointSize, colors=colors[1],drawstyle=1))
#         self.tipPNHandler.append(self._orEnv.plot3(points=c2, pointsize=pointSize, colors=colors[2],drawstyle=1))
# 
#         self.tipPNHandler.append(self._orEnv.drawarrow(p1=c0, p2=c0 + 0.02 * n0,linewidth=0.001,color=colors[0]))
#         self.tipPNHandler.append(self._orEnv.drawarrow(p1=c1, p2=c1 + 0.02 * n1,linewidth=0.001,color=colors[1]))
#         self.tipPNHandler.append(self._orEnv.drawarrow(p1=c2, p2=c2 + 0.02 * n2,linewidth=0.001,color=colors[2]))
# 
# 
#     def getSiblingLabel(self, label):
#         if len(label) <= self._hops / 2:
#             ret = []
#             for i in range(len(label)):
#                 ret.append(np.random.randint(self._levels[i] + 1))
#         else:
#             matchLen = len(label) - self._hops / 2
#             ret = label[:matchLen]
#             for i in range(len(label) - matchLen):
#                 ret.append(np.random.randint(self._levels[i + matchLen] + 1))
#         return ret
# 
# 
#     def getSiblingLabels(self, currLabels, allowedFingerCombos=None):
# 
#         labels_tmp = []
#         if allowedFingerCombos is None:
#             for i in range(self._contactN):
#                 tmp = []
#                 # while tmp in labels_tmp or len(tmp) == 0:
#                 while len(tmp) == 0:
#                     tmp = self.getSiblingLabel(currLabels[i])
#                 labels_tmp.append(tmp)
#         else:
#             fingerCombo = random.choice(allowedFingerCombos)
#             for i in range(self._contactN):
#                 tmp = list(currLabels[i])
#                 tmp[-1] = fingerCombo[i]
#                 labels_tmp.append(tmp)
#         return labels_tmp
# 
#     def getMaximumDepth(self):
#         return self._nLevel
# 
#     def setAlpha(self, a):
#         assert a > 0
#         self._alpha = a
# 
#     def setMaxIter(self, m):
#         assert m > 0
#         self._maxIters = m
# 
#     def getRootNode(self):
#         possibleNumChildren, possibleNumLeaves = self.getBranchInformation(0)
#         return HFTSNode(possibleNumChildren=possibleNumChildren,
#                         possibleNumLeaves=possibleNumLeaves)
# 
# class HFTSNode:
#     def __init__(self, labels=None, handConf=None, preGraspHandConfig=None,
#                  armConf=None, goal=False, leaf=False, valid=False,
#                  possibleNumChildren=0, possibleNumLeaves=0, quality=0.0):
#         # None values represent the root node
# 
#         if labels is None:
#             self._depth = 0
#         else:
#             self._depth = len(labels[0])
# 
#         self._labels = labels
#         self._handConfig = handConf
#         self._preGraspHandConfig = preGraspHandConfig
#         self._possibleNumLeaves = possibleNumLeaves
#         self._possibleNumChildren = possibleNumChildren
#         self._armConfig = armConf
#         self._goal = goal
#         self._bIsLeaf = leaf
#         self._valid = valid
#         self._quality = quality
# 
#     def getLabels(self):
#         return self._labels
# 
#     def getUniqueLabel(self):
#         if self._labels is None:
#             return 'root'
#         label = []
#         for fingerLabel in self._labels:
#             label.extend(fingerLabel)
#         return str(label)
# 
#     def isExtendible(self):
#         return not self._bIsLeaf
# 
#     def getContactLabels(self):
#         return self._labels
# 
#     def isLeaf(self):
#         return self._bIsLeaf
# 
#     def getDepth(self):
#         return self._depth
# 
#     def getHandConfig(self):
#         return self._handConfig
# 
#     def getPreGraspHandConfig(self):
#         return self._preGraspHandConfig
# 
#     def getArmConfig(self):
#         return self._armConfig
# 
#     def getPossibleNumChildren(self):
#         return self._possibleNumChildren
# 
#     def getPossibleNumLeaves(self):
#         return self._possibleNumLeaves
# 
#     def isGoal(self):
#         return self._goal
# 
#     def isValid(self):
#         return self._valid
# 
#     def getQuality(self):
#         return self._quality
# 
#     def hasConfiguration(self):
#         return self._armConfig is not None and self._handConfig is not None
# 