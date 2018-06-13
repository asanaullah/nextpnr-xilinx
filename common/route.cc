/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Clifford Wolf <clifford@clifford.at>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <cmath>
#include <queue>

#include "log.h"
#include "route.h"

NEXTPNR_NAMESPACE_BEGIN

struct QueuedWire
{
    WireId wire;
    PipId pip;

    float delay = 0, togo = 0;

    struct Greater
    {
        bool operator()(const QueuedWire &lhs, const QueuedWire &rhs) const
                noexcept
        {
            return (lhs.delay + lhs.togo) > (rhs.delay + rhs.togo);
        }
    };
};

void route_design(Design *design, bool verbose)
{
    auto &chip = design->chip;
    int itercnt = 0, netcnt = 0;
    float maxDelay = 0.0;

    log_info("Routing..\n");

    for (auto &net_it : design->nets) {
        auto net_name = net_it.first;
        auto net_info = net_it.second;

        if (net_info->driver.cell == nullptr)
            continue;

        if (verbose)
            log("Routing net %s.\n", net_name.c_str());
        netcnt++;

        if (verbose)
            log("  Source: %s.%s.\n", net_info->driver.cell->name.c_str(),
                net_info->driver.port.c_str());

        auto src_bel = net_info->driver.cell->bel;
        auto src_pos = chip.getBelPosition(src_bel);

        if (src_bel == BelId())
            log_error("Source cell is not mapped to a bel.\n");

        if (verbose)
            log("    Source bel: %s\n", chip.getBelName(src_bel).c_str());

        auto src_wire = chip.getWireBelPin(
                src_bel, portPinFromId(net_info->driver.port));

        if (src_wire == WireId())
            log_error("No wire found for port %s on source bel.\n",
                      net_info->driver.port.c_str());

        if (verbose)
            log("    Source wire: %s\n", chip.getWireName(src_wire).c_str());

        std::unordered_map<WireId, DelayInfo> src_wires;
        src_wires[src_wire] = DelayInfo();
        net_info->wires[src_wire] = PipId();
        chip.bindWire(src_wire, net_name);

        for (auto &user_it : net_info->users) {
            if (verbose)
                log("  Route to: %s.%s.\n", user_it.cell->name.c_str(),
                    user_it.port.c_str());

            auto dst_bel = user_it.cell->bel;
            auto dst_pos = chip.getBelPosition(dst_bel);

            if (dst_bel == BelId())
                log_error("Destination cell is not mapped to a bel.\n");

            if (verbose) {
                log("    Destination bel: %s\n",
                    chip.getBelName(dst_bel).c_str());
                log("    Path delay estimate: %.2f\n",
                    chip.estimateDelay(src_pos, dst_pos));
            }

            auto dst_wire =
                    chip.getWireBelPin(dst_bel, portPinFromId(user_it.port));

            if (dst_wire == WireId())
                log_error("No wire found for port %s on destination bel.\n",
                          user_it.port.c_str());

            if (verbose)
                log("    Destination wire: %s\n",
                    chip.getWireName(dst_wire).c_str());

            std::unordered_map<WireId, QueuedWire> visited;
            std::priority_queue<QueuedWire, std::vector<QueuedWire>,
                                QueuedWire::Greater>
                    queue;

            for (auto &it : src_wires) {
                QueuedWire qw;
                qw.wire = it.first;
                qw.pip = PipId();
                qw.delay = it.second.avgDelay();
                qw.togo = chip.estimateDelay(chip.getWirePosition(qw.wire),
                                             dst_pos);

                queue.push(qw);
                visited[qw.wire] = qw;
            }

            while (!queue.empty()) {
                itercnt++;
                QueuedWire qw = queue.top();
                queue.pop();

                for (auto pip : chip.getPipsDownhill(qw.wire)) {
                    if (!chip.checkPipAvail(pip))
                        continue;

                    WireId next_wire = chip.getPipDstWire(pip);
                    float next_delay =
                            qw.delay + chip.getPipDelay(pip).avgDelay();

                    if (visited.count(next_wire)) {
                        if (visited.at(next_wire).delay <= next_delay + 1e-3)
                            continue;
                        if (verbose)
                            log("Found better route to %s. Old vs new delay "
                                "estimate: %.2f %.2f\n",
                                chip.getWireName(next_wire).c_str(),
                                visited.at(next_wire).delay, next_delay);
                    }

                    if (!chip.checkWireAvail(next_wire))
                        continue;

                    QueuedWire next_qw;
                    next_qw.wire = next_wire;
                    next_qw.pip = pip;
                    next_qw.delay = next_delay;
                    next_qw.togo = chip.estimateDelay(
                            chip.getWirePosition(next_wire), dst_pos);
                    visited[next_qw.wire] = next_qw;
                    queue.push(next_qw);

                    if (next_qw.wire == dst_wire) {
                        std::priority_queue<QueuedWire, std::vector<QueuedWire>,
                                            QueuedWire::Greater>
                                empty_queue;
                        std::swap(queue, empty_queue);
                        break;
                    }
                }
            }

            if (visited.count(dst_wire) == 0)
                log_error("Failed to route %s -> %s.\n",
                          chip.getWireName(src_wire).c_str(),
                          chip.getWireName(dst_wire).c_str());

            if (verbose)
                log("    Final path delay: %.2f\n", visited[dst_wire].delay);
            maxDelay = fmaxf(maxDelay, visited[dst_wire].delay);

            if (verbose)
                log("    Route (from destination to source):\n");

            WireId cursor = dst_wire;

            while (1) {
                if (verbose)
                    log("    %8.2f %s\n", visited[cursor].delay,
                        chip.getWireName(cursor).c_str());

                if (src_wires.count(cursor))
                    break;

                net_info->wires[cursor] = visited[cursor].pip;
                chip.bindWire(cursor, net_name);
                chip.bindPip(visited[cursor].pip, net_name);

                src_wires[cursor] = chip.getPipDelay(visited[cursor].pip);
                cursor = chip.getPipSrcWire(visited[cursor].pip);
            }
        }
    }

    log_info("routed %d nets, visited %d wires.\n", netcnt, itercnt);
    log_info("longest path delay: %.2f\n", maxDelay);
}

NEXTPNR_NAMESPACE_END