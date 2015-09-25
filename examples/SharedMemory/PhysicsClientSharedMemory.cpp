#include "PhysicsClientSharedMemory.h"
#include "PosixSharedMemory.h"
#include "Win32SharedMemory.h"
#include "LinearMath/btAlignedObjectArray.h"
#include "LinearMath/btVector3.h"

#include "Bullet3Common/b3Logging.h"
#include "../Utils/b3ResourcePath.h"
#include "../../Extras/Serialize/BulletFileLoader/btBulletFile.h"
#include "../../Extras/Serialize/BulletFileLoader/autogenerated/bullet.h"
#include "SharedMemoryBlock.h"

// copied from btMultiBodyLink.h
enum JointType {
    eRevoluteType = 0,
    ePrismaticType = 1,
};

struct TmpFloat3 {
    float m_x;
    float m_y;
    float m_z;
};

TmpFloat3 CreateTmpFloat3(float x, float y, float z) {
    TmpFloat3 tmp;
    tmp.m_x = x;
    tmp.m_y = y;
    tmp.m_z = z;
    return tmp;
}

struct PhysicsClientSharedMemoryInternalData {
    SharedMemoryInterface* m_sharedMemory;
    SharedMemoryBlock* m_testBlock1;

    btAlignedObjectArray<bParse::btBulletFile*> m_robotMultiBodyData;
    btAlignedObjectArray<b3JointInfo> m_jointInfo;
    btAlignedObjectArray<TmpFloat3> m_debugLinesFrom;
    btAlignedObjectArray<TmpFloat3> m_debugLinesTo;
    btAlignedObjectArray<TmpFloat3> m_debugLinesColor;

    SharedMemoryStatus m_lastServerStatus;

    int m_counter;
    bool m_serverLoadUrdfOK;
    bool m_isConnected;
    bool m_waitingForServer;
    bool m_hasLastServerStatus;
    int m_sharedMemoryKey;
    bool m_verboseOutput;

    PhysicsClientSharedMemoryInternalData()
        : m_sharedMemory(0),
          m_testBlock1(0),
          m_counter(0),
          m_serverLoadUrdfOK(false),
          m_isConnected(false),
          m_waitingForServer(false),
          m_hasLastServerStatus(false),
          m_sharedMemoryKey(SHARED_MEMORY_KEY),
          m_verboseOutput(false) {}

    void processServerStatus();

    bool canSubmitCommand() const;
};

int PhysicsClientSharedMemory::getNumJoints() const { return m_data->m_jointInfo.size(); }

void PhysicsClientSharedMemory::getJointInfo(int index, b3JointInfo& info) const {
    info = m_data->m_jointInfo[index];
}

PhysicsClientSharedMemory::PhysicsClientSharedMemory()

{
    m_data = new PhysicsClientSharedMemoryInternalData;

#ifdef _WIN32
    m_data->m_sharedMemory = new Win32SharedMemoryClient();
#else
    m_data->m_sharedMemory = new PosixSharedMemory();
#endif
}

PhysicsClientSharedMemory::~PhysicsClientSharedMemory() {
    if (m_data->m_isConnected) {
        disconnectSharedMemory();
    }
    delete m_data->m_sharedMemory;
    delete m_data;
}

void PhysicsClientSharedMemory::setSharedMemoryKey(int key) { m_data->m_sharedMemoryKey = key; }

void PhysicsClientSharedMemory::disconnectSharedMemory() {
    if (m_data->m_isConnected) {
        m_data->m_sharedMemory->releaseSharedMemory(m_data->m_sharedMemoryKey, SHARED_MEMORY_SIZE);
        m_data->m_isConnected = false;
    }
}

bool PhysicsClientSharedMemory::isConnected() const { return m_data->m_isConnected; }

bool PhysicsClientSharedMemory::connect() {
    /// server always has to create and initialize shared memory
    bool allowCreation = false;
    m_data->m_testBlock1 = (SharedMemoryBlock*)m_data->m_sharedMemory->allocateSharedMemory(
        m_data->m_sharedMemoryKey, SHARED_MEMORY_SIZE, allowCreation);

    if (m_data->m_testBlock1) {
        if (m_data->m_testBlock1->m_magicId != SHARED_MEMORY_MAGIC_NUMBER) {
            b3Error("Error: please start server before client\n");
            m_data->m_sharedMemory->releaseSharedMemory(m_data->m_sharedMemoryKey,
                                                        SHARED_MEMORY_SIZE);
            m_data->m_testBlock1 = 0;
            return false;
        } else {
            if (m_data->m_verboseOutput) {
                b3Printf("Connected to existing shared memory, status OK.\n");
            }
            m_data->m_isConnected = true;
        }
    } else {
        b3Error("Cannot connect to shared memory");
        return false;
    }
    return true;
}

const SharedMemoryStatus* PhysicsClientSharedMemory::processServerStatus() {
    SharedMemoryStatus* stat = 0;

    if (!m_data->m_testBlock1) {
        return 0;
    }

    if (!m_data->m_waitingForServer) {
        return 0;
    }

    if (m_data->m_testBlock1->m_numServerCommands >
        m_data->m_testBlock1->m_numProcessedServerCommands) {
        btAssert(m_data->m_testBlock1->m_numServerCommands ==
                 m_data->m_testBlock1->m_numProcessedServerCommands + 1);

        const SharedMemoryStatus& serverCmd = m_data->m_testBlock1->m_serverCommands[0];
        m_data->m_lastServerStatus = serverCmd;

        EnumSharedMemoryServerStatus s = (EnumSharedMemoryServerStatus)serverCmd.m_type;
        // consume the command
        switch (serverCmd.m_type) {
            case CMD_CLIENT_COMMAND_COMPLETED: {
                if (m_data->m_verboseOutput) {
                    b3Printf("Server completed command");
                }
                break;
            }
            case CMD_URDF_LOADING_COMPLETED: {
                m_data->m_serverLoadUrdfOK = true;
                if (m_data->m_verboseOutput) {
                    b3Printf("Server loading the URDF OK\n");
                }

                if (serverCmd.m_dataStreamArguments.m_streamChunkLength > 0) {
                    bParse::btBulletFile* bf = new bParse::btBulletFile(
                        this->m_data->m_testBlock1->m_bulletStreamDataServerToClient,
                        serverCmd.m_dataStreamArguments.m_streamChunkLength);
                    bf->setFileDNAisMemoryDNA();
                    bf->parse(false);
                    m_data->m_robotMultiBodyData.push_back(bf);

                    for (int i = 0; i < bf->m_multiBodies.size(); i++) {
                        int flag = bf->getFlags();
                        int qOffset = 7;
                        int uOffset = 6;

                        if ((flag & bParse::FD_DOUBLE_PRECISION) != 0) {
                            Bullet::btMultiBodyDoubleData* mb =
                                (Bullet::btMultiBodyDoubleData*)bf->m_multiBodies[i];
                            if (mb->m_baseName) {
                                if (m_data->m_verboseOutput) {
                                    b3Printf("mb->m_baseName = %s\n", mb->m_baseName);
                                }
                            }

                            for (int link = 0; link < mb->m_numLinks; link++) {
                                {
                                    b3JointInfo info;
                                    info.m_flags = 0;
                                    info.m_jointIndex = link;
                                    info.m_qIndex =
                                        (0 < mb->m_links[link].m_posVarCount) ? qOffset : -1;
                                    info.m_uIndex =
                                        (0 < mb->m_links[link].m_dofCount) ? uOffset : -1;

                                    if (mb->m_links[link].m_linkName) {
                                        if (m_data->m_verboseOutput) {
                                            b3Printf("mb->m_links[%d].m_linkName = %s\n", link,
                                                     mb->m_links[link].m_linkName);
                                        }
                                        info.m_linkName = mb->m_links[link].m_linkName;
                                    }
                                    if (mb->m_links[link].m_jointName) {
                                        if (m_data->m_verboseOutput) {
                                            b3Printf("mb->m_links[%d].m_jointName = %s\n", link,
                                                     mb->m_links[link].m_jointName);
                                        }
                                        info.m_jointName = mb->m_links[link].m_jointName;
                                        info.m_jointType = mb->m_links[link].m_jointType;
                                    }
                                    if ((mb->m_links[link].m_jointType == eRevoluteType) ||
                                        (mb->m_links[link].m_jointType == ePrismaticType)) {
                                        info.m_flags |= JOINT_HAS_MOTORIZED_POWER;
                                    }
                                    m_data->m_jointInfo.push_back(info);
                                }
                                qOffset += mb->m_links[link].m_posVarCount;
                                uOffset += mb->m_links[link].m_dofCount;
                            }

                        } else {
                            Bullet::btMultiBodyFloatData* mb =
                                (Bullet::btMultiBodyFloatData*)bf->m_multiBodies[i];
                            if (mb->m_baseName) {
                                if (m_data->m_verboseOutput) {
                                    b3Printf("mb->m_baseName = %s\n", mb->m_baseName);
                                }
                            }
                            for (int link = 0; link < mb->m_numLinks; link++) {
                                {
                                    b3JointInfo info;
                                    info.m_flags = 0;
                                    info.m_jointIndex = link;
                                    info.m_qIndex =
                                        (0 < mb->m_links[link].m_posVarCount) ? qOffset : -1;
                                    info.m_uIndex =
                                        (0 < mb->m_links[link].m_dofCount) ? uOffset : -1;

                                    if (mb->m_links[link].m_linkName) {
                                        if (m_data->m_verboseOutput) {
                                            b3Printf("mb->m_links[%d].m_linkName = %s\n", link,
                                                     mb->m_links[link].m_linkName);
                                        }
                                        info.m_linkName = mb->m_links[link].m_linkName;
                                    }
                                    if (mb->m_links[link].m_jointName) {
                                        if (m_data->m_verboseOutput) {
                                            b3Printf("mb->m_links[%d].m_jointName = %s\n", link,
                                                     mb->m_links[link].m_jointName);
                                        }
                                        info.m_jointName = mb->m_links[link].m_jointName;
                                        info.m_jointType = mb->m_links[link].m_jointType;
                                    }
                                    if ((mb->m_links[link].m_jointType == eRevoluteType) ||
                                        (mb->m_links[link].m_jointType == ePrismaticType)) {
                                        info.m_flags |= JOINT_HAS_MOTORIZED_POWER;
                                    }
                                    m_data->m_jointInfo.push_back(info);
                                }
                                qOffset += mb->m_links[link].m_posVarCount;
                                uOffset += mb->m_links[link].m_dofCount;
                            }
                        }
                    }
                    if (bf->ok()) {
                        if (m_data->m_verboseOutput) {
                            b3Printf("Received robot description ok!\n");
                        }
                    } else {
                        b3Warning("Robot description not received");
                    }
                }
                break;
            }
            case CMD_DESIRED_STATE_RECEIVED_COMPLETED: {
                if (m_data->m_verboseOutput) {
                    b3Printf("Server received desired state");
                }
                break;
            }
            case CMD_STEP_FORWARD_SIMULATION_COMPLETED: {
                if (m_data->m_verboseOutput) {
                    b3Printf("Server completed step simulation");
                }
                break;
            }
            case CMD_URDF_LOADING_FAILED: {
                if (m_data->m_verboseOutput) {
                    b3Printf("Server failed loading the URDF...\n");
                }
                m_data->m_serverLoadUrdfOK = false;
                break;
            }

            case CMD_BULLET_DATA_STREAM_RECEIVED_COMPLETED: {
                if (m_data->m_verboseOutput) {
                    b3Printf("Server received bullet data stream OK\n");
                }

                break;
            }
            case CMD_BULLET_DATA_STREAM_RECEIVED_FAILED: {
                if (m_data->m_verboseOutput) {
                    b3Printf("Server failed receiving bullet data stream\n");
                }

                break;
            }

            case CMD_ACTUAL_STATE_UPDATE_COMPLETED: {
                if (m_data->m_verboseOutput) {
                    b3Printf("Received actual state\n");
                }
                SharedMemoryStatus& command = m_data->m_testBlock1->m_serverCommands[0];

                int numQ = command.m_sendActualStateArgs.m_numDegreeOfFreedomQ;
                int numU = command.m_sendActualStateArgs.m_numDegreeOfFreedomU;
                if (m_data->m_verboseOutput) {
                    b3Printf("size Q = %d, size U = %d\n", numQ, numU);
                }
                char msg[1024];

                {
                    sprintf(msg, "Q=[");

                    for (int i = 0; i < numQ; i++) {
                        if (i < numQ - 1) {
                            sprintf(msg, "%s%f,", msg,
                                    command.m_sendActualStateArgs.m_actualStateQ[i]);
                        } else {
                            sprintf(msg, "%s%f", msg,
                                    command.m_sendActualStateArgs.m_actualStateQ[i]);
                        }
                    }
                    sprintf(msg, "%s]", msg);
                }
                if (m_data->m_verboseOutput) {
                    b3Printf(msg);
                }

                {
                    sprintf(msg, "U=[");

                    for (int i = 0; i < numU; i++) {
                        if (i < numU - 1) {
                            sprintf(msg, "%s%f,", msg,
                                    command.m_sendActualStateArgs.m_actualStateQdot[i]);
                        } else {
                            sprintf(msg, "%s%f", msg,
                                    command.m_sendActualStateArgs.m_actualStateQdot[i]);
                        }
                    }
                    sprintf(msg, "%s]", msg);
                }
                if (m_data->m_verboseOutput) {
                    b3Printf(msg);
                }

                if (m_data->m_verboseOutput) {
                    b3Printf("\n");
                }
                break;
            }
            case CMD_RESET_SIMULATION_COMPLETED: {
                if (m_data->m_verboseOutput) {
                    b3Printf("CMD_RESET_SIMULATION_COMPLETED clean data\n");
                }
                for (int i = 0; i < m_data->m_robotMultiBodyData.size(); i++) {
                    delete m_data->m_robotMultiBodyData[i];
                }
                m_data->m_robotMultiBodyData.clear();

                m_data->m_jointInfo.clear();
                break;
            }
            case CMD_DEBUG_LINES_COMPLETED: {
                if (m_data->m_verboseOutput) {
                    b3Printf("Success receiving %d debug lines",
                             serverCmd.m_sendDebugLinesArgs.m_numDebugLines);
                }

                int numLines = serverCmd.m_sendDebugLinesArgs.m_numDebugLines;
                float* linesFrom =
                    (float*)&m_data->m_testBlock1->m_bulletStreamDataServerToClient[0];
                float* linesTo =
                    (float*)(&m_data->m_testBlock1->m_bulletStreamDataServerToClient[0] +
                             numLines * 3 * sizeof(float));
                float* linesColor =
                    (float*)(&m_data->m_testBlock1->m_bulletStreamDataServerToClient[0] +
                             2 * numLines * 3 * sizeof(float));

                m_data->m_debugLinesFrom.resize(serverCmd.m_sendDebugLinesArgs.m_startingLineIndex +
                                                numLines);
                m_data->m_debugLinesTo.resize(serverCmd.m_sendDebugLinesArgs.m_startingLineIndex +
                                              numLines);
                m_data->m_debugLinesColor.resize(
                    serverCmd.m_sendDebugLinesArgs.m_startingLineIndex + numLines);

                for (int i = 0; i < numLines; i++) {
                    TmpFloat3 from = CreateTmpFloat3(linesFrom[i * 3], linesFrom[i * 3 + 1],
                                                     linesFrom[i * 3 + 2]);
                    TmpFloat3 to =
                        CreateTmpFloat3(linesTo[i * 3], linesTo[i * 3 + 1], linesTo[i * 3 + 2]);
                    TmpFloat3 color = CreateTmpFloat3(linesColor[i * 3], linesColor[i * 3 + 1],
                                                      linesColor[i * 3 + 2]);

                    m_data
                        ->m_debugLinesFrom[serverCmd.m_sendDebugLinesArgs.m_startingLineIndex + i] =
                        from;
                    m_data->m_debugLinesTo[serverCmd.m_sendDebugLinesArgs.m_startingLineIndex + i] =
                        to;
                    m_data->m_debugLinesColor[serverCmd.m_sendDebugLinesArgs.m_startingLineIndex +
                                              i] = color;
                }

                break;
            }
            case CMD_DEBUG_LINES_OVERFLOW_FAILED: {
                b3Warning("Error receiving debug lines");
                m_data->m_debugLinesFrom.resize(0);
                m_data->m_debugLinesTo.resize(0);
                m_data->m_debugLinesColor.resize(0);

                break;
            }

            default: {
                b3Error("Unknown server status\n");
                btAssert(0);
            }
        };

        m_data->m_testBlock1->m_numProcessedServerCommands++;
        // we don't have more than 1 command outstanding (in total, either server or client)
        btAssert(m_data->m_testBlock1->m_numProcessedServerCommands ==
                 m_data->m_testBlock1->m_numServerCommands);

        if (m_data->m_testBlock1->m_numServerCommands ==
            m_data->m_testBlock1->m_numProcessedServerCommands) {
            m_data->m_waitingForServer = false;
        } else {
            m_data->m_waitingForServer = true;
        }

        if ((serverCmd.m_type == CMD_DEBUG_LINES_COMPLETED) &&
            (serverCmd.m_sendDebugLinesArgs.m_numRemainingDebugLines > 0)) {
            SharedMemoryCommand& command = m_data->m_testBlock1->m_clientCommands[0];

            // continue requesting debug lines for drawing
            command.m_type = CMD_REQUEST_DEBUG_LINES;
            command.m_requestDebugLinesArguments.m_startingLineIndex =
                serverCmd.m_sendDebugLinesArgs.m_numDebugLines +
                serverCmd.m_sendDebugLinesArgs.m_startingLineIndex;
            submitClientCommand(command);
            return 0;
        }

        return &m_data->m_lastServerStatus;

    } else {
        if (m_data->m_verboseOutput) {
            b3Printf("m_numServerStatus  = %d, processed = %d\n",
                     m_data->m_testBlock1->m_numServerCommands,
                     m_data->m_testBlock1->m_numProcessedServerCommands);
        }
    }
    return 0;
}

bool PhysicsClientSharedMemory::canSubmitCommand() const {
    return (m_data->m_isConnected && !m_data->m_waitingForServer);
}

struct SharedMemoryCommand* PhysicsClientSharedMemory::getAvailableSharedMemoryCommand() {
    return &m_data->m_testBlock1->m_clientCommands[0];
}

bool PhysicsClientSharedMemory::submitClientCommand(const SharedMemoryCommand& command) {
    /// at the moment we allow a maximum of 1 outstanding command, so we check for this
    // once the server processed the command and returns a status, we clear the flag
    // "m_data->m_waitingForServer" and allow submitting the next command
    btAssert(!m_data->m_waitingForServer);

    if (!m_data->m_waitingForServer) {
        if (&m_data->m_testBlock1->m_clientCommands[0] != &command) {
            m_data->m_testBlock1->m_clientCommands[0] = command;
        }
        m_data->m_testBlock1->m_numClientCommands++;
        m_data->m_waitingForServer = true;
        return true;
    }
    return false;
}

void PhysicsClientSharedMemory::uploadBulletFileToSharedMemory(const char* data, int len) {
    btAssert(len < SHARED_MEMORY_MAX_STREAM_CHUNK_SIZE);
    if (len >= SHARED_MEMORY_MAX_STREAM_CHUNK_SIZE) {
        b3Warning("uploadBulletFileToSharedMemory %d exceeds max size %d\n", len,
                  SHARED_MEMORY_MAX_STREAM_CHUNK_SIZE);
    } else {
        for (int i = 0; i < len; i++) {
            m_data->m_testBlock1->m_bulletStreamDataClientToServer[i] = data[i];
        }
    }
}

const float* PhysicsClientSharedMemory::getDebugLinesFrom() const {
    if (m_data->m_debugLinesFrom.size()) {
        return &m_data->m_debugLinesFrom[0].m_x;
    }
    return 0;
}
const float* PhysicsClientSharedMemory::getDebugLinesTo() const {
    if (m_data->m_debugLinesTo.size()) {
        return &m_data->m_debugLinesTo[0].m_x;
    }
    return 0;
}
const float* PhysicsClientSharedMemory::getDebugLinesColor() const {
    if (m_data->m_debugLinesColor.size()) {
        return &m_data->m_debugLinesColor[0].m_x;
    }
    return 0;
}
int PhysicsClientSharedMemory::getNumDebugLines() const { return m_data->m_debugLinesFrom.size(); }
