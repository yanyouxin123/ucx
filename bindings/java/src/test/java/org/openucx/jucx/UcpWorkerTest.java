/*
 * Copyright (C) Mellanox Technologies Ltd. 2001-2019.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

package org.openucx.jucx;

import org.junit.Test;
import org.openucx.jucx.ucp.*;
import org.openucx.jucx.ucs.UcsConstants;

import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;

import static org.junit.Assert.*;

public class UcpWorkerTest {
    private static int numWorkers = Runtime.getRuntime().availableProcessors();

    @Test
    public void testSingleWorker() {
        UcpContext context = new UcpContext(new UcpParams().requestTagFeature());
        assertEquals(2, UcsConstants.ThreadMode.UCS_THREAD_MODE_MULTI);
        assertNotEquals(context.getNativeId(), null);
        UcpWorker worker = context.newWorker(new UcpWorkerParams());
        assertNotNull(worker.getNativeId());
        assertEquals(worker.progress(), 0); // No communications was submitted.
        worker.close();
        assertNull(worker.getNativeId());
        context.close();
    }

    @Test
    public void testMultipleWorkersWithinSameContext() {
        UcpContext context = new UcpContext(new UcpParams().requestTagFeature());
        assertNotEquals(context.getNativeId(), null);
        UcpWorker[] workers = new UcpWorker[numWorkers];
        UcpWorkerParams workerParam = new UcpWorkerParams();
        for (int i = 0; i < numWorkers; i++) {
            workerParam.clear().setCpu(i).requestThreadSafety();
            workers[i] = context.newWorker(workerParam);
            assertNotNull(workers[i].getNativeId());
        }
        for (int i = 0; i < numWorkers; i++) {
            workers[i].close();
        }
        context.close();
    }

    @Test
    public void testMultipleWorkersFromMultipleContexts() {
        UcpContext tcpContext = new UcpContext(new UcpParams().requestTagFeature());
        UcpContext rdmaContext = new UcpContext(new UcpParams().requestRmaFeature()
            .requestAtomic64BitFeature().requestAtomic32BitFeature());
        UcpWorker[] workers = new UcpWorker[numWorkers];
        UcpWorkerParams workerParams = new UcpWorkerParams();
        for (int i = 0; i < numWorkers; i++) {
            ByteBuffer userData = ByteBuffer.allocateDirect(100);
            workerParams.clear();
            if (i % 2 == 0) {
                userData.asCharBuffer().put("TCPWorker" + i);
                workerParams.requestWakeupRX().setUserData(userData);
                workers[i] = tcpContext.newWorker(workerParams);
            } else {
                userData.asCharBuffer().put("RDMAWorker" + i);
                workerParams.requestWakeupRMA().setCpu(i).setUserData(userData)
                    .requestThreadSafety();
                workers[i] = rdmaContext.newWorker(workerParams);
            }
        }
        for (int i = 0; i < numWorkers; i++) {
            workers[i].close();
        }
        tcpContext.close();
        rdmaContext.close();
    }

    @Test
    public void testGetWorkerAddress() {
        UcpContext context = new UcpContext(new UcpParams().requestTagFeature());
        UcpWorker worker = context.newWorker(new UcpWorkerParams());
        ByteBuffer workerAddress = worker.getAddress();
        assertNotNull(workerAddress);
        assertTrue(workerAddress.capacity() > 0);
        worker.close();
        context.close();
    }

    @Test
    public void testWorkerSleepWakeup() throws InterruptedException {
        UcpContext context = new UcpContext(new UcpParams()
            .requestRmaFeature().requestWakeupFeature());
        UcpWorker worker = context.newWorker(
            new UcpWorkerParams().requestWakeupRMA());

        AtomicBoolean success = new AtomicBoolean(false);
        Thread workerProgressThread = new Thread() {
            @Override
            public void run() {
                while (!isInterrupted()) {
                    if (worker.progress() == 0) {
                        worker.waitForEvents();
                    }
                }
                success.set(true);
            }
        };

        workerProgressThread.start();

        workerProgressThread.interrupt();
        worker.signal();

        workerProgressThread.join();
        assertTrue(success.get());

        worker.close();
        context.close();
    }

    @Test
    public void testFlushWorker() {
        int numRequests = 10;
        // Crerate 2 contexts + 2 workers
        UcpParams params = new UcpParams().requestRmaFeature();
        UcpWorkerParams rdmaWorkerParams = new UcpWorkerParams().requestWakeupRMA();
        UcpContext context1 = new UcpContext(params);
        UcpContext context2 = new UcpContext(params);

        ByteBuffer src = ByteBuffer.allocateDirect(UcpMemoryTest.MEM_SIZE);
        src.asCharBuffer().put(UcpMemoryTest.RANDOM_TEXT);
        ByteBuffer dst = ByteBuffer.allocateDirect(UcpMemoryTest.MEM_SIZE);
        UcpMemory memory = context2.registerMemory(src);

        UcpWorker worker1 = context1.newWorker(rdmaWorkerParams);
        UcpWorker worker2 = context2.newWorker(rdmaWorkerParams);

        UcpEndpoint ep = worker1.newEndpoint( new UcpEndpointParams()
            .setUcpAddress(worker2.getAddress()).setPeerErrorHadnlingMode());
        UcpRemoteKey rkey = ep.unpackRemoteKey(memory.getRemoteKeyBuffer());

        int blockSize = UcpMemoryTest.MEM_SIZE / numRequests;
        for (int i = 0; i < numRequests; i++) {
            ep.getNonBlocking(memory.getAddress() + i * blockSize, rkey,
                   UcxUtils.getAddress(dst) + i * blockSize, blockSize, null);
        }

        UcxRequest request = worker1.flushNonBlocking(new UcxCallback() {
            @Override
            public void onSuccess(UcxRequest request) {
                rkey.close();
                memory.deregister();
                assertEquals(dst.asCharBuffer().toString().trim(), UcpMemoryTest.RANDOM_TEXT);
            }
        });

        while (request.isCompleted()) {
            worker1.progress();
            worker2.progress();
        }

        ep.close();
        worker1.close();
        worker2.close();
        context1.close();
        context2.close();
    }
}
