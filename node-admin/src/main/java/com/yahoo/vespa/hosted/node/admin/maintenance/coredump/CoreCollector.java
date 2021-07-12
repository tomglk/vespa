// Copyright 2018 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.vespa.hosted.node.admin.maintenance.coredump;

import com.yahoo.vespa.hosted.node.admin.container.ContainerOperations;
import com.yahoo.vespa.hosted.node.admin.nodeadmin.ConvergenceException;
import com.yahoo.vespa.hosted.node.admin.nodeagent.NodeAgentContext;
import com.yahoo.vespa.hosted.node.admin.task.util.process.CommandResult;

import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Takes in an uncompressed core dump and collects relevant metadata.
 *
 * @author freva
 */
public class CoreCollector {
    private static final Logger logger = Logger.getLogger(CoreCollector.class.getName());

    private static final Pattern JAVA_HEAP_DUMP_PATTERN = Pattern.compile("java_pid.*\\.hprof$");
    private static final Pattern CORE_GENERATOR_PATH_PATTERN = Pattern.compile("^Core was generated by `(?<path>.*?)'.$");
    private static final Pattern EXECFN_PATH_PATTERN = Pattern.compile("^.* execfn: '(?<path>.*?)'");
    private static final Pattern FROM_PATH_PATTERN = Pattern.compile("^.* from '(?<path>.*?)'");
    static final String GDB_PATH_RHEL7_DT9 = "/opt/rh/devtoolset-9/root/bin/gdb";
    static final String GDB_PATH_RHEL7_DT10 = "/opt/rh/devtoolset-10/root/bin/gdb";
    static final String GDB_PATH_RHEL8 = "/opt/rh/gcc-toolset-10/root/bin/gdb";

    static final Map<String, Object> JAVA_HEAP_DUMP_METADATA =
            Map.of("bin_path", "java", "backtrace", List.of("Heap dump, no backtrace available"));

    private final ContainerOperations docker;

    public CoreCollector(ContainerOperations docker) {
        this.docker = docker;
    }

    String getGdbPath(NodeAgentContext context) {
        // TODO: Remove when we do not have any devtoolset-9 installs left
        String[] command_rhel7_dt9 = {"stat", GDB_PATH_RHEL7_DT9};
        if (docker.executeCommandInContainerAsRoot(context, command_rhel7_dt9).getExitCode() == 0) {
            return GDB_PATH_RHEL7_DT9;
        }

        String[] command_rhel7_dt10 = {"stat", GDB_PATH_RHEL7_DT10};
        if (docker.executeCommandInContainerAsRoot(context, command_rhel7_dt10).getExitCode() == 0) {
            return GDB_PATH_RHEL7_DT10;
        }

        return GDB_PATH_RHEL8;
    }

    Path readBinPathFallback(NodeAgentContext context, Path coredumpPath) {
        String command = getGdbPath(context) + " -n -batch -core " + coredumpPath + " | grep \'^Core was generated by\'";
        String[] wrappedCommand = {"/bin/sh", "-c", command};
        CommandResult result = docker.executeCommandInContainerAsRoot(context, wrappedCommand);

        Matcher matcher = CORE_GENERATOR_PATH_PATTERN.matcher(result.getOutput());
        if (! matcher.find()) {
            throw new ConvergenceException(String.format("Failed to extract binary path from GDB, result: %s, command: %s",
                    asString(result), Arrays.toString(wrappedCommand)));
        }
        return Paths.get(matcher.group("path").split(" ")[0]);
    }

    Path readBinPath(NodeAgentContext context, Path coredumpPath) {
        String[] command = {"file", coredumpPath.toString()};
        try {
            CommandResult result = docker.executeCommandInContainerAsRoot(context, command);
            if (result.getExitCode() != 0) {
                throw new ConvergenceException("file command failed with " + asString(result));
            }

            Matcher execfnMatcher = EXECFN_PATH_PATTERN.matcher(result.getOutput());
            if (execfnMatcher.find()) {
                return Paths.get(execfnMatcher.group("path").split(" ")[0]);
            }

            Matcher fromMatcher = FROM_PATH_PATTERN.matcher(result.getOutput());
            if (fromMatcher.find()) {
                return Paths.get(fromMatcher.group("path").split(" ")[0]);
            }
        } catch (RuntimeException e) {
            context.log(logger, Level.WARNING, String.format("Failed getting bin path, command: %s. " +
                    "Trying fallback instead", Arrays.toString(command)), e);
        }

        return readBinPathFallback(context, coredumpPath);
    }

    List<String> readBacktrace(NodeAgentContext context, Path coredumpPath, Path binPath, boolean allThreads) {
        String threads = allThreads ? "thread apply all bt" : "bt";
        String[] command = {getGdbPath(context), "-n", "-ex", threads, "-batch", binPath.toString(), coredumpPath.toString()};

        CommandResult result = docker.executeCommandInContainerAsRoot(context, command);
        if (result.getExitCode() != 0)
            throw new ConvergenceException("Failed to read backtrace " + asString(result) + ", Command: " + Arrays.toString(command));

        return List.of(result.getOutput().split("\n"));
    }

    List<String> readJstack(NodeAgentContext context, Path coredumpPath, Path binPath) {
        String[] command = {"jhsdb", "jstack", "--exe", binPath.toString(), "--core", coredumpPath.toString()};

        CommandResult result = docker.executeCommandInContainerAsRoot(context, command);
        if (result.getExitCode() != 0)
            throw new ConvergenceException("Failed to read jstack " + asString(result) + ", Command: " + Arrays.toString(command));

        return List.of(result.getOutput().split("\n"));
    }

    /**
     * Collects metadata about a given core dump
     * @param context context of the NodeAgent that owns the core dump
     * @param coredumpPath path to core dump file inside the container
     * @return map of relevant metadata about the core dump
     */
    Map<String, Object> collect(NodeAgentContext context, Path coredumpPath) {
        if (JAVA_HEAP_DUMP_PATTERN.matcher(coredumpPath.getFileName().toString()).find())
            return JAVA_HEAP_DUMP_METADATA;

        Map<String, Object> data = new HashMap<>();
        try {
            Path binPath = readBinPath(context, coredumpPath);

            data.put("bin_path", binPath.toString());
            if (binPath.getFileName().toString().equals("java")) {
                data.put("backtrace_all_threads", readJstack(context, coredumpPath, binPath));
            } else {
                data.put("backtrace", readBacktrace(context, coredumpPath, binPath, false));
                data.put("backtrace_all_threads", readBacktrace(context, coredumpPath, binPath, true));
            }
        } catch (ConvergenceException e) {
            context.log(logger, Level.WARNING, "Failed to extract backtrace: " + e.getMessage());
        } catch (RuntimeException e) {
            context.log(logger, Level.WARNING, "Failed to extract backtrace", e);
        }
        return data;
    }

    private String asString(CommandResult result) {
        return "exit status " + result.getExitCode() + ", output '" + result.getOutput() + "'";
    }

}
