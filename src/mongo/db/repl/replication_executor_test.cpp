/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <map>
#include <boost/thread/thread.hpp>

#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/map_util.h"

namespace mongo {
namespace repl {

namespace {

    template <typename T>
    const T& ASSERT_GET(const StatusWith<T>& v) {
        ASSERT_OK(v.getStatus());
        return v.getValue();
    }

    bool operator==(const ReplicationExecutor::RemoteCommandRequest lhs,
                    const ReplicationExecutor::RemoteCommandRequest rhs) {
        return lhs.target == rhs.target &&
            lhs.dbname == rhs.dbname &&
            lhs.cmdObj == rhs.cmdObj;
    }

    bool operator!=(const ReplicationExecutor::RemoteCommandRequest lhs,
                    const ReplicationExecutor::RemoteCommandRequest rhs) {
        return !(lhs == rhs);
    }

    void setStatus(ReplicationExecutor* executor, const Status& source, Status* target) {
        *target = source;
    }

    void setStatusAndShutdown(ReplicationExecutor* executor,
                              const Status& source,
                              Status* target) {
        setStatus(executor, source, target);
        if (source != ErrorCodes::CallbackCanceled)
            executor->shutdown();
    }

    void setStatusAndTriggerEvent(ReplicationExecutor* executor,
                                  const Status& inStatus,
                                  Status* outStatus,
                                  ReplicationExecutor::EventHandle event) {
        *outStatus = inStatus;
        if (!inStatus.isOK())
            return;
        executor->signalEvent(event);
    }

    void scheduleSetStatusAndShutdown(ReplicationExecutor* executor,
                                      const Status& inStatus,
                                      Status* outStatus1,
                                      Status* outStatus2) {
        if (!inStatus.isOK()) {
            *outStatus1 = inStatus;
            return;
        }
        *outStatus1= executor->scheduleWork(stdx::bind(setStatusAndShutdown,
                                                       stdx::placeholders::_1,
                                                       stdx::placeholders::_2,
                                                       outStatus2)).getStatus();
    }

    TEST(ReplicationExecutor, RunOne) {
        ReplicationExecutor executor(new NetworkInterfaceMock);
        Status status(ErrorCodes::InternalError, "Not mutated");
        ASSERT_OK(executor.scheduleWork(stdx::bind(setStatusAndShutdown,
                                                   stdx::placeholders::_1,
                                                   stdx::placeholders::_2,
                                                   &status)).getStatus());
        executor.run();
        ASSERT_OK(status);
    }

    TEST(ReplicationExecutor, Schedule1ButShutdown) {
        ReplicationExecutor executor(new NetworkInterfaceMock);
        Status status(ErrorCodes::InternalError, "Not mutated");
        ASSERT_OK(executor.scheduleWork(stdx::bind(setStatusAndShutdown,
                                                   stdx::placeholders::_1,
                                                   stdx::placeholders::_2,
                                                   &status)).getStatus());
        executor.shutdown();
        executor.run();
        ASSERT_EQUALS(status, ErrorCodes::CallbackCanceled);
    }

    TEST(ReplicationExecutor, Schedule2Cancel1) {
        ReplicationExecutor executor(new NetworkInterfaceMock);
        Status status1(ErrorCodes::InternalError, "Not mutated");
        Status status2(ErrorCodes::InternalError, "Not mutated");
        ReplicationExecutor::CallbackHandle cb = ASSERT_GET(
            executor.scheduleWork(stdx::bind(setStatusAndShutdown,
                                             stdx::placeholders::_1,
                                             stdx::placeholders::_2,
                                             &status1)));
        executor.cancel(cb);
        ASSERT_OK(executor.scheduleWork(stdx::bind(setStatusAndShutdown,
                                                   stdx::placeholders::_1,
                                                   stdx::placeholders::_2,
                                                   &status2)).getStatus());
        executor.run();
        ASSERT_EQUALS(status1, ErrorCodes::CallbackCanceled);
        ASSERT_OK(status2);
    }

    TEST(ReplicationExecutor, OneSchedulesAnother) {
        ReplicationExecutor executor(new NetworkInterfaceMock);
        Status status1(ErrorCodes::InternalError, "Not mutated");
        Status status2(ErrorCodes::InternalError, "Not mutated");
        ASSERT_OK(executor.scheduleWork(stdx::bind(scheduleSetStatusAndShutdown,
                                                   stdx::placeholders::_1,
                                                   stdx::placeholders::_2,
                                                   &status1,
                                                   &status2)).getStatus());
        executor.run();
        ASSERT_OK(status1);
        ASSERT_OK(status2);
    }

    class EventChainAndWaitingTest {
        MONGO_DISALLOW_COPYING(EventChainAndWaitingTest);
    public:
        EventChainAndWaitingTest();
        void run();
    private:
        void onGo(ReplicationExecutor* executor, const Status& status);
        void onGoAfterTriggered(ReplicationExecutor* executor,
                                const Status& status);

        ReplicationExecutor executor;
        boost::thread executorThread;
        const ReplicationExecutor::EventHandle goEvent;
        const ReplicationExecutor::EventHandle event2;
        const ReplicationExecutor::EventHandle event3;
        ReplicationExecutor::EventHandle triggerEvent;
        ReplicationExecutor::CallbackFn  triggered2;
        ReplicationExecutor::CallbackFn  triggered3;
        Status status1;
        Status status2;
        Status status3;
        Status status4;
        Status status5;
    };

    TEST(ReplicationExecutor, EventChainAndWaiting) {
        EventChainAndWaitingTest().run();
    }

    EventChainAndWaitingTest::EventChainAndWaitingTest() :
        executor(new NetworkInterfaceMock),
        executorThread(stdx::bind(&ReplicationExecutor::run, &executor)),
        goEvent(ASSERT_GET(executor.makeEvent())),
        event2(ASSERT_GET(executor.makeEvent())),
        event3(ASSERT_GET(executor.makeEvent())),
        status1(ErrorCodes::InternalError, "Not mutated"),
        status2(ErrorCodes::InternalError, "Not mutated"),
        status3(ErrorCodes::InternalError, "Not mutated"),
        status4(ErrorCodes::InternalError, "Not mutated"),
        status5(ErrorCodes::InternalError, "Not mutated") {

        triggered2 = stdx::bind(setStatusAndTriggerEvent,
                                stdx::placeholders::_1,
                                stdx::placeholders::_2,
                                &status2,
                                event2);
        triggered3 = stdx::bind(setStatusAndTriggerEvent,
                                stdx::placeholders::_1,
                                stdx::placeholders::_2,
                                &status3,
                                event3);
    }

    void EventChainAndWaitingTest::run() {
        executor.onEvent(goEvent,
                         stdx::bind(&EventChainAndWaitingTest::onGo,
                                    this,
                                    stdx::placeholders::_1,
                                    stdx::placeholders::_2));
        executor.signalEvent(goEvent);
        executor.waitForEvent(goEvent);
        executor.waitForEvent(event2);
        executor.waitForEvent(event3);

        ReplicationExecutor::EventHandle neverSignaledEvent = ASSERT_GET(executor.makeEvent());
        boost::thread neverSignaledWaiter(stdx::bind(&ReplicationExecutor::waitForEvent,
                                                     &executor,
                                                     neverSignaledEvent));
        ReplicationExecutor::CallbackHandle shutdownCallback = ASSERT_GET(
                executor.scheduleWork(stdx::bind(setStatusAndShutdown,
                                                 stdx::placeholders::_1,
                                                 stdx::placeholders::_2,
                                                 &status5)));
        executor.wait(shutdownCallback);
        neverSignaledWaiter.join();
        executorThread.join();
        ASSERT_OK(status1);
        ASSERT_OK(status2);
        ASSERT_OK(status3);
        ASSERT_OK(status4);
        ASSERT_OK(status5);
    }

    void EventChainAndWaitingTest::onGo(ReplicationExecutor* executor,
                                        const Status& status) {
        if (!status.isOK()) {
            status1 = status;
            return;
        }
        StatusWith<ReplicationExecutor::EventHandle> errorOrTriggerEvent = executor->makeEvent();
        if (!errorOrTriggerEvent.isOK()) {
            status1 = errorOrTriggerEvent.getStatus();
            executor->shutdown();
            return;
        }
        triggerEvent = errorOrTriggerEvent.getValue();
        StatusWith<ReplicationExecutor::CallbackHandle> cbHandle = executor->onEvent(
                triggerEvent, triggered2);
        if (!cbHandle.isOK()) {
            status1 = cbHandle.getStatus();
            executor->shutdown();
            return;
        }
        cbHandle = executor->onEvent(triggerEvent, triggered3);
        if (!cbHandle.isOK()) {
            status1 = cbHandle.getStatus();
            executor->shutdown();
            return;
        }

        cbHandle = executor->onEvent(
                goEvent,
                stdx::bind(&EventChainAndWaitingTest::onGoAfterTriggered,
                           this,
                           stdx::placeholders::_1,
                           stdx::placeholders::_2));
        if (!cbHandle.isOK()) {
            status1 = cbHandle.getStatus();
            executor->shutdown();
            return;
        }
        status1 = Status::OK();
    }

    void EventChainAndWaitingTest::onGoAfterTriggered(ReplicationExecutor* executor,
                                                      const Status& status) {
        status4 = status;
        if (!status.isOK()) {
            return;
        }
        executor->signalEvent(triggerEvent);
    }

    TEST(ReplicationExecutor, ScheduleWorkAt) {
        NetworkInterfaceMock* net = new NetworkInterfaceMock;
        ReplicationExecutor executor(net);
        Status status1(ErrorCodes::InternalError, "Not mutated");
        Status status2(ErrorCodes::InternalError, "Not mutated");
        Status status3(ErrorCodes::InternalError, "Not mutated");
        const Date_t now = net->now();
        ASSERT_GET(executor.scheduleWorkAt(Date_t(now.millis + 100),
                                           stdx::bind(setStatus,
                                                      stdx::placeholders::_1,
                                                      stdx::placeholders::_2,
                                                      &status1)));
        ASSERT_GET(executor.scheduleWorkAt(Date_t(now.millis + 5000),
                                           stdx::bind(setStatus,
                                                      stdx::placeholders::_1,
                                                      stdx::placeholders::_2,
                                                      &status3)));
        ASSERT_GET(executor.scheduleWorkAt(Date_t(now.millis + 200),
                                           stdx::bind(setStatusAndShutdown,
                                                      stdx::placeholders::_1,
                                                      stdx::placeholders::_2,
                                                      &status2)));
        executor.run();
        ASSERT_OK(status1);
        ASSERT_OK(status2);
        ASSERT_EQUALS(status3, ErrorCodes::CallbackCanceled);
    }

    std::string getRequestDescription(const ReplicationExecutor::RemoteCommandRequest& request) {
        return mongoutils::str::stream() << "Request(" << request.target.toString() << ", " <<
            request.dbname << ", " << request.cmdObj << ')';
    }

    static void setStatusOnRemoteCommandCompletion(
            ReplicationExecutor* executor,
            const ReplicationExecutor::RemoteCommandRequest& actualRequest,
            const StatusWith<BSONObj>& actualResponse,
            const ReplicationExecutor::RemoteCommandRequest& expectedRequest,
            Status* outStatus) {

        if (actualRequest != expectedRequest) {
            *outStatus = Status(
                    ErrorCodes::BadValue,
                    mongoutils::str::stream() << "Actual request: " <<
                    getRequestDescription(actualRequest) << "; expected: " <<
                    getRequestDescription(expectedRequest));
            return;
        }
        *outStatus = actualResponse.getStatus();
    }

    TEST(ReplicationExecutor, ScheduleRemoteCommand) {
        NetworkInterfaceMock* net = new NetworkInterfaceMock;
        ReplicationExecutor executor(net);
        Status status1(ErrorCodes::InternalError, "Not mutated");
        const ReplicationExecutor::RemoteCommandRequest request(
                HostAndPort("localhost", 27017),
                "mydb",
                BSON("whatsUp" << "doc"));
        ReplicationExecutor::CallbackHandle cbHandle = ASSERT_GET(
                executor.scheduleRemoteCommand(
                        request,
                        stdx::bind(setStatusOnRemoteCommandCompletion,
                                   stdx::placeholders::_1,
                                   stdx::placeholders::_2,
                                   stdx::placeholders::_3,
                                   request,
                                   &status1)));
        boost::thread executorThread(stdx::bind(&ReplicationExecutor::run, &executor));
        executor.wait(cbHandle);
        executor.shutdown();
        executorThread.join();
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, status1);
    }

    TEST(ReplicationExecutor, ScheduleAndCancelRemoteCommand) {
        NetworkInterfaceMock* net = new NetworkInterfaceMock;
        ReplicationExecutor executor(net);
        Status status1(ErrorCodes::InternalError, "Not mutated");
        const ReplicationExecutor::RemoteCommandRequest request(
                HostAndPort("localhost", 27017),
                "mydb",
                BSON("whatsUp" << "doc"));
        ReplicationExecutor::CallbackHandle cbHandle = ASSERT_GET(
                executor.scheduleRemoteCommand(
                        request,
                        stdx::bind(setStatusOnRemoteCommandCompletion,
                                   stdx::placeholders::_1,
                                   stdx::placeholders::_2,
                                   stdx::placeholders::_3,
                                   request,
                                   &status1)));
        executor.cancel(cbHandle);
        boost::thread executorThread(stdx::bind(&ReplicationExecutor::run, &executor));
        executor.wait(cbHandle);
        executor.shutdown();
        executorThread.join();
        ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status1);
    }

    TEST(ReplicationExecutor, ScheduleExclusiveLockOperation) {
        ReplicationExecutor executor(new NetworkInterfaceMock);
        Status status1(ErrorCodes::InternalError, "Not mutated");
        ASSERT_OK(executor.scheduleWorkWithGlobalExclusiveLock(
                          stdx::bind(setStatusAndShutdown,
                                     stdx::placeholders::_1,
                                     stdx::placeholders::_2,
                                     &status1)).getStatus());
        executor.run();
        ASSERT_OK(status1);
    }

}  // namespace
}  // namespace repl
}  // namespace mongo
