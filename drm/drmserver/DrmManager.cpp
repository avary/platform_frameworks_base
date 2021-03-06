/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "DrmManager(Native)"
#include "utils/Log.h"

#include <utils/String8.h>
#include <drm/DrmInfo.h>
#include <drm/DrmInfoEvent.h>
#include <drm/DrmRights.h>
#include <drm/DrmConstraints.h>
#include <drm/DrmMetadata.h>
#include <drm/DrmInfoStatus.h>
#include <drm/DrmInfoRequest.h>
#include <drm/DrmSupportInfo.h>
#include <drm/DrmConvertedStatus.h>
#include <IDrmEngine.h>

#include "DrmManager.h"
#include "ReadWriteUtils.h"

#define DECRYPT_FILE_ERROR -1

using namespace android;

Vector<int> DrmManager::mUniqueIdVector;
const String8 DrmManager::EMPTY_STRING("");

DrmManager::DrmManager() :
    mDecryptSessionId(0),
    mConvertId(0) {

}

DrmManager::~DrmManager() {

}

int DrmManager::addUniqueId(int uniqueId) {
    if (0 == uniqueId) {
        int temp = 0;
        bool foundUniqueId = false;
        srand(time(NULL));

        while (!foundUniqueId) {
            const int size = mUniqueIdVector.size();
            temp = rand() % 100;

            int index = 0;
            for (; index < size; ++index) {
                if (mUniqueIdVector.itemAt(index) == temp) {
                    foundUniqueId = false;
                    break;
                }
            }
            if (index == size) {
                foundUniqueId = true;
            }
        }
        uniqueId = temp;
    }
    mUniqueIdVector.push(uniqueId);
    return uniqueId;
}

void DrmManager::removeUniqueId(int uniqueId) {
    for (unsigned int i = 0; i < mUniqueIdVector.size(); i++) {
        if (uniqueId == mUniqueIdVector.itemAt(i)) {
            mUniqueIdVector.removeAt(i);
            break;
        }
    }
}

status_t DrmManager::loadPlugIns() {
    String8 pluginDirPath("/system/lib/drm/plugins/native");
    return loadPlugIns(pluginDirPath);
}

status_t DrmManager::loadPlugIns(const String8& plugInDirPath) {
    if (mSupportInfoToPlugInIdMap.isEmpty()) {
        mPlugInManager.loadPlugIns(plugInDirPath);
        Vector<String8> plugInPathList = mPlugInManager.getPlugInIdList();
        for (unsigned int i = 0; i < plugInPathList.size(); ++i) {
            String8 plugInPath = plugInPathList[i];
            DrmSupportInfo* info = mPlugInManager.getPlugIn(plugInPath).getSupportInfo(0);
            if (NULL != info) {
                mSupportInfoToPlugInIdMap.add(*info, plugInPath);
            }
        }
    }
    return DRM_NO_ERROR;
}

status_t DrmManager::unloadPlugIns() {
    mConvertSessionMap.clear();
    mDecryptSessionMap.clear();
    mPlugInManager.unloadPlugIns();
    mSupportInfoToPlugInIdMap.clear();
    return DRM_NO_ERROR;
}

status_t DrmManager::setDrmServiceListener(
            int uniqueId, const sp<IDrmServiceListener>& drmServiceListener) {
    Mutex::Autolock _l(mLock);
    if (NULL != drmServiceListener.get()) {
        mServiceListeners.add(uniqueId, drmServiceListener);
    } else {
        mServiceListeners.removeItem(uniqueId);
    }
    return DRM_NO_ERROR;
}

void DrmManager::addClient(int uniqueId) {
    if (!mSupportInfoToPlugInIdMap.isEmpty()) {
        Vector<String8> plugInIdList = mPlugInManager.getPlugInIdList();
        for (unsigned int index = 0; index < plugInIdList.size(); index++) {
            IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInIdList.itemAt(index));
            rDrmEngine.initialize(uniqueId);
            rDrmEngine.setOnInfoListener(uniqueId, this);
        }
    }
}

void DrmManager::removeClient(int uniqueId) {
    Vector<String8> plugInIdList = mPlugInManager.getPlugInIdList();
    for (unsigned int index = 0; index < plugInIdList.size(); index++) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInIdList.itemAt(index));
        rDrmEngine.terminate(uniqueId);
    }
}

DrmConstraints* DrmManager::getConstraints(int uniqueId, const String8* path, const int action) {
    const String8 plugInId = getSupportedPlugInIdFromPath(uniqueId, *path);
    if (EMPTY_STRING != plugInId) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
        return rDrmEngine.getConstraints(uniqueId, path, action);
    }
    return NULL;
}

DrmMetadata* DrmManager::getMetadata(int uniqueId, const String8* path) {
    const String8 plugInId = getSupportedPlugInIdFromPath(uniqueId, *path);
    if (EMPTY_STRING != plugInId) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
        return rDrmEngine.getMetadata(uniqueId, path);
    }
    return NULL;
}

status_t DrmManager::installDrmEngine(int uniqueId, const String8& absolutePath) {
    mPlugInManager.loadPlugIn(absolutePath);

    IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(absolutePath);
    rDrmEngine.initialize(uniqueId);
    rDrmEngine.setOnInfoListener(uniqueId, this);

    DrmSupportInfo* info = rDrmEngine.getSupportInfo(0);
    mSupportInfoToPlugInIdMap.add(*info, absolutePath);

    return DRM_NO_ERROR;
}

bool DrmManager::canHandle(int uniqueId, const String8& path, const String8& mimeType) {
    const String8 plugInId = getSupportedPlugInId(mimeType);
    bool result = (EMPTY_STRING != plugInId) ? true : false;

    if (0 < path.length()) {
        if (result) {
            IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
            result = rDrmEngine.canHandle(uniqueId, path);
        } else {
            result = canHandle(uniqueId, path);
        }
    }
    return result;
}

DrmInfoStatus* DrmManager::processDrmInfo(int uniqueId, const DrmInfo* drmInfo) {
    const String8 plugInId = getSupportedPlugInId(drmInfo->getMimeType());
    if (EMPTY_STRING != plugInId) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
        return rDrmEngine.processDrmInfo(uniqueId, drmInfo);
    }
    return NULL;
}

bool DrmManager::canHandle(int uniqueId, const String8& path) {
    bool result = false;
    Vector<String8> plugInPathList = mPlugInManager.getPlugInIdList();

    for (unsigned int i = 0; i < plugInPathList.size(); ++i) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInPathList[i]);
        result = rDrmEngine.canHandle(uniqueId, path);

        if (result) {
            break;
        }
    }
    return result;
}

DrmInfo* DrmManager::acquireDrmInfo(int uniqueId, const DrmInfoRequest* drmInfoRequest) {
    const String8 plugInId = getSupportedPlugInId(drmInfoRequest->getMimeType());
    if (EMPTY_STRING != plugInId) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
        return rDrmEngine.acquireDrmInfo(uniqueId, drmInfoRequest);
    }
    return NULL;
}

status_t DrmManager::saveRights(int uniqueId, const DrmRights& drmRights,
            const String8& rightsPath, const String8& contentPath) {
    const String8 plugInId = getSupportedPlugInId(drmRights.getMimeType());
    status_t result = DRM_ERROR_UNKNOWN;
    if (EMPTY_STRING != plugInId) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
        result = rDrmEngine.saveRights(uniqueId, drmRights, rightsPath, contentPath);
    }
    return result;
}

String8 DrmManager::getOriginalMimeType(int uniqueId, const String8& path) {
    const String8 plugInId = getSupportedPlugInIdFromPath(uniqueId, path);
    if (EMPTY_STRING != plugInId) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
        return rDrmEngine.getOriginalMimeType(uniqueId, path);
    }
    return EMPTY_STRING;
}

int DrmManager::getDrmObjectType(int uniqueId, const String8& path, const String8& mimeType) {
    const String8 plugInId = getSupportedPlugInId(uniqueId, path, mimeType);
    if (EMPTY_STRING != plugInId) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
        return rDrmEngine.getDrmObjectType(uniqueId, path, mimeType);
    }
    return DrmObjectType::UNKNOWN;
}

int DrmManager::checkRightsStatus(int uniqueId, const String8& path, int action) {
    const String8 plugInId = getSupportedPlugInIdFromPath(uniqueId, path);
    if (EMPTY_STRING != plugInId) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
        return rDrmEngine.checkRightsStatus(uniqueId, path, action);
    }
    return RightsStatus::RIGHTS_INVALID;
}

status_t DrmManager::consumeRights(
    int uniqueId, DecryptHandle* decryptHandle, int action, bool reserve) {
    status_t result = DRM_ERROR_UNKNOWN;
    if (mDecryptSessionMap.indexOfKey(decryptHandle->decryptId) != NAME_NOT_FOUND) {
        IDrmEngine* drmEngine = mDecryptSessionMap.valueFor(decryptHandle->decryptId);
        result = drmEngine->consumeRights(uniqueId, decryptHandle, action, reserve);
    }
    return result;
}

status_t DrmManager::setPlaybackStatus(
    int uniqueId, DecryptHandle* decryptHandle, int playbackStatus, int position) {
    status_t result = DRM_ERROR_UNKNOWN;
    if (mDecryptSessionMap.indexOfKey(decryptHandle->decryptId) != NAME_NOT_FOUND) {
        IDrmEngine* drmEngine = mDecryptSessionMap.valueFor(decryptHandle->decryptId);
        result = drmEngine->setPlaybackStatus(uniqueId, decryptHandle, playbackStatus, position);
    }
    return result;
}

bool DrmManager::validateAction(
    int uniqueId, const String8& path, int action, const ActionDescription& description) {
    const String8 plugInId = getSupportedPlugInIdFromPath(uniqueId, path);
    if (EMPTY_STRING != plugInId) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
        return rDrmEngine.validateAction(uniqueId, path, action, description);
    }
    return false;
}

status_t DrmManager::removeRights(int uniqueId, const String8& path) {
    const String8 plugInId = getSupportedPlugInIdFromPath(uniqueId, path);
    status_t result = DRM_ERROR_UNKNOWN;
    if (EMPTY_STRING != plugInId) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
        result = rDrmEngine.removeRights(uniqueId, path);
    }
    return result;
}

status_t DrmManager::removeAllRights(int uniqueId) {
    Vector<String8> plugInIdList = mPlugInManager.getPlugInIdList();
    status_t result = DRM_ERROR_UNKNOWN;
    for (unsigned int index = 0; index < plugInIdList.size(); index++) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInIdList.itemAt(index));
        result = rDrmEngine.removeAllRights(uniqueId);
        if (DRM_NO_ERROR != result) {
            break;
        }
    }
    return result;
}

int DrmManager::openConvertSession(int uniqueId, const String8& mimeType) {
    int convertId = -1;

    const String8 plugInId = getSupportedPlugInId(mimeType);
    if (EMPTY_STRING != plugInId) {
        IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);

        if (DRM_NO_ERROR == rDrmEngine.openConvertSession(uniqueId, mConvertId + 1)) {
            Mutex::Autolock _l(mConvertLock);
            ++mConvertId;
            convertId = mConvertId;
            mConvertSessionMap.add(convertId, &rDrmEngine);
        }
    }
    return convertId;
}

DrmConvertedStatus* DrmManager::convertData(
            int uniqueId, int convertId, const DrmBuffer* inputData) {
    DrmConvertedStatus *drmConvertedStatus = NULL;

    if (mConvertSessionMap.indexOfKey(convertId) != NAME_NOT_FOUND) {
        IDrmEngine* drmEngine = mConvertSessionMap.valueFor(convertId);
        drmConvertedStatus = drmEngine->convertData(uniqueId, convertId, inputData);
    }
    return drmConvertedStatus;
}

DrmConvertedStatus* DrmManager::closeConvertSession(int uniqueId, int convertId) {
    DrmConvertedStatus *drmConvertedStatus = NULL;

    if (mConvertSessionMap.indexOfKey(convertId) != NAME_NOT_FOUND) {
        IDrmEngine* drmEngine = mConvertSessionMap.valueFor(convertId);
        drmConvertedStatus = drmEngine->closeConvertSession(uniqueId, convertId);
        mConvertSessionMap.removeItem(convertId);
    }
    return drmConvertedStatus;
}

status_t DrmManager::getAllSupportInfo(
                    int uniqueId, int* length, DrmSupportInfo** drmSupportInfoArray) {
    Vector<String8> plugInPathList = mPlugInManager.getPlugInIdList();
    int size = plugInPathList.size();
    int validPlugins = 0;

    if (0 < size) {
        Vector<DrmSupportInfo> drmSupportInfoList;

        for (int i = 0; i < size; ++i) {
            String8 plugInPath = plugInPathList[i];
            DrmSupportInfo* drmSupportInfo
                = mPlugInManager.getPlugIn(plugInPath).getSupportInfo(0);
            if (NULL != drmSupportInfo) {
                drmSupportInfoList.add(*drmSupportInfo);
                delete drmSupportInfo; drmSupportInfo = NULL;
            }
        }

        validPlugins = drmSupportInfoList.size();
        if (0 < validPlugins) {
            *drmSupportInfoArray = new DrmSupportInfo[validPlugins];
            for (int i = 0; i < validPlugins; ++i) {
                (*drmSupportInfoArray)[i] = drmSupportInfoList[i];
            }
        }
    }
    *length = validPlugins;
    return DRM_NO_ERROR;
}

DecryptHandle* DrmManager::openDecryptSession(int uniqueId, int fd, int offset, int length) {
    Mutex::Autolock _l(mDecryptLock);
    status_t result = DRM_ERROR_CANNOT_HANDLE;
    Vector<String8> plugInIdList = mPlugInManager.getPlugInIdList();

    DecryptHandle* handle = new DecryptHandle();
    if (NULL != handle) {
        handle->decryptId = mDecryptSessionId + 1;

        for (unsigned int index = 0; index < plugInIdList.size(); index++) {
            String8 plugInId = plugInIdList.itemAt(index);
            IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
            result = rDrmEngine.openDecryptSession(uniqueId, handle, fd, offset, length);

            if (DRM_NO_ERROR == result) {
                ++mDecryptSessionId;
                mDecryptSessionMap.add(mDecryptSessionId, &rDrmEngine);
                break;
            }
        }
    }
    if (DRM_NO_ERROR != result) {
        delete handle; handle = NULL;
        LOGE("DrmManager::openDecryptSession: no capable plug-in found");
    }
    return handle;
}

DecryptHandle* DrmManager::openDecryptSession(int uniqueId, const char* uri) {
    Mutex::Autolock _l(mDecryptLock);
    status_t result = DRM_ERROR_CANNOT_HANDLE;
    Vector<String8> plugInIdList = mPlugInManager.getPlugInIdList();

    DecryptHandle* handle = new DecryptHandle();
    if (NULL != handle) {
        handle->decryptId = mDecryptSessionId + 1;

        for (unsigned int index = 0; index < plugInIdList.size(); index++) {
            String8 plugInId = plugInIdList.itemAt(index);
            IDrmEngine& rDrmEngine = mPlugInManager.getPlugIn(plugInId);
            result = rDrmEngine.openDecryptSession(uniqueId, handle, uri);

            if (DRM_NO_ERROR == result) {
                ++mDecryptSessionId;
                mDecryptSessionMap.add(mDecryptSessionId, &rDrmEngine);
                break;
            }
        }
    }
    if (DRM_NO_ERROR != result) {
        delete handle; handle = NULL;
        LOGE("DrmManager::openDecryptSession: no capable plug-in found");
    }
    return handle;
}

status_t DrmManager::closeDecryptSession(int uniqueId, DecryptHandle* decryptHandle) {
    Mutex::Autolock _l(mDecryptLock);
    status_t result = DRM_ERROR_UNKNOWN;
    if (mDecryptSessionMap.indexOfKey(decryptHandle->decryptId) != NAME_NOT_FOUND) {
        IDrmEngine* drmEngine = mDecryptSessionMap.valueFor(decryptHandle->decryptId);
        result = drmEngine->closeDecryptSession(uniqueId, decryptHandle);
        if (DRM_NO_ERROR == result) {
            mDecryptSessionMap.removeItem(decryptHandle->decryptId);
        }
    }
    return result;
}

status_t DrmManager::initializeDecryptUnit(
    int uniqueId, DecryptHandle* decryptHandle, int decryptUnitId, const DrmBuffer* headerInfo) {
    status_t result = DRM_ERROR_UNKNOWN;
    if (mDecryptSessionMap.indexOfKey(decryptHandle->decryptId) != NAME_NOT_FOUND) {
        IDrmEngine* drmEngine = mDecryptSessionMap.valueFor(decryptHandle->decryptId);
        result = drmEngine->initializeDecryptUnit(uniqueId, decryptHandle, decryptUnitId, headerInfo);
    }
    return result;
}

status_t DrmManager::decrypt(int uniqueId, DecryptHandle* decryptHandle, int decryptUnitId,
            const DrmBuffer* encBuffer, DrmBuffer** decBuffer, DrmBuffer* IV) {
    status_t result = DRM_ERROR_UNKNOWN;
    if (mDecryptSessionMap.indexOfKey(decryptHandle->decryptId) != NAME_NOT_FOUND) {
        IDrmEngine* drmEngine = mDecryptSessionMap.valueFor(decryptHandle->decryptId);
        result = drmEngine->decrypt(
                uniqueId, decryptHandle, decryptUnitId, encBuffer, decBuffer, IV);
    }
    return result;
}

status_t DrmManager::finalizeDecryptUnit(
            int uniqueId, DecryptHandle* decryptHandle, int decryptUnitId) {
    status_t result = DRM_ERROR_UNKNOWN;
    if (mDecryptSessionMap.indexOfKey(decryptHandle->decryptId) != NAME_NOT_FOUND) {
        IDrmEngine* drmEngine = mDecryptSessionMap.valueFor(decryptHandle->decryptId);
        result = drmEngine->finalizeDecryptUnit(uniqueId, decryptHandle, decryptUnitId);
    }
    return result;
}

ssize_t DrmManager::pread(int uniqueId, DecryptHandle* decryptHandle,
            void* buffer, ssize_t numBytes, off_t offset) {
    ssize_t result = DECRYPT_FILE_ERROR;

    if (mDecryptSessionMap.indexOfKey(decryptHandle->decryptId) != NAME_NOT_FOUND) {
        IDrmEngine* drmEngine = mDecryptSessionMap.valueFor(decryptHandle->decryptId);
        result = drmEngine->pread(uniqueId, decryptHandle, buffer, numBytes, offset);
    }
    return result;
}

String8 DrmManager::getSupportedPlugInId(
            int uniqueId, const String8& path, const String8& mimeType) {
    String8 plugInId("");

    if (EMPTY_STRING != mimeType) {
        plugInId = getSupportedPlugInId(mimeType);
    } else {
        plugInId = getSupportedPlugInIdFromPath(uniqueId, path);
    }
    return plugInId;
}

String8 DrmManager::getSupportedPlugInId(const String8& mimeType) {
    String8 plugInId("");

    if (EMPTY_STRING != mimeType) {
        for (unsigned int index = 0; index < mSupportInfoToPlugInIdMap.size(); index++) {
            const DrmSupportInfo& drmSupportInfo = mSupportInfoToPlugInIdMap.keyAt(index);

            if (drmSupportInfo.isSupportedMimeType(mimeType)) {
                plugInId = mSupportInfoToPlugInIdMap.valueFor(drmSupportInfo);
                break;
            }
        }
    }
    return plugInId;
}

String8 DrmManager::getSupportedPlugInIdFromPath(int uniqueId, const String8& path) {
    String8 plugInId("");
    const String8 fileSuffix = path.getPathExtension();

    for (unsigned int index = 0; index < mSupportInfoToPlugInIdMap.size(); index++) {
        const DrmSupportInfo& drmSupportInfo = mSupportInfoToPlugInIdMap.keyAt(index);

        if (drmSupportInfo.isSupportedFileSuffix(fileSuffix)) {
            String8 key = mSupportInfoToPlugInIdMap.valueFor(drmSupportInfo);
            IDrmEngine& drmEngine = mPlugInManager.getPlugIn(key);

            if (drmEngine.canHandle(uniqueId, path)) {
                plugInId = key;
                break;
            }
        }
    }
    return plugInId;
}

void DrmManager::onInfo(const DrmInfoEvent& event) {
    Mutex::Autolock _l(mLock);
    for (unsigned int index = 0; index < mServiceListeners.size(); index++) {
        int uniqueId = mServiceListeners.keyAt(index);

        if (uniqueId == event.getUniqueId()) {
            sp<IDrmServiceListener> serviceListener = mServiceListeners.valueFor(uniqueId);
            serviceListener->notify(event);
        }
    }
}

