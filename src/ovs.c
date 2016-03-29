#define _GNU_SOURCE

#include "ovs.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"
#include "mem.h"
#include "net.h"

struct ovsContext_s {
	netContext* net;
	char* directory;
	char* dbSocketConnArg;
};

static int switchContext(ovsContext* ctx) {
	return netSwitchNamespace(ctx->net);
}

#define OVS_DEFAULT_SCHEMA_PATH "/usr/share/openvswitch/vswitch.ovsschema"
#define OVS_CMD_MAX_ARGS 20
#define OVSDB_CTL_FILE "ovsdb-server.ctl"
#define OVS_CTL_FILE "ovs-vswitchd.ctl"

// Forks and executes an OVS command with the given arguments. The first
// argument is automatically set to be the command. Must call switchContext
// before this function. If output is non-NULL, output, outputLen, and
// outputCap are a flexBuffer that will contain the merger of stdout and stderr
// for the subprocess. Returns 0 on success or an error code otherwise.
static int ovsCommand(char** output, size_t* outputLen, size_t* outputCap, int expectStatus, ...) {
	char* argv[OVS_CMD_MAX_ARGS+2];
	va_list args;
	va_start(args, expectStatus);
	char* command = va_arg(args, char*); // Safe cast (see POSIX standard)

	lprintHead(LogDebug);
	argv[0] = command;
	lprintDirectf(LogDebug, "Running Open vSwitch command: %s", command);
	size_t argCount;
	for (argCount = 1; argCount < OVS_CMD_MAX_ARGS+1; ++argCount) {
		argv[argCount] = va_arg(args, char*); // Also a safe cast
		if (argv[argCount] == NULL) break;
		lprintDirectf(LogDebug, " %s", argv[argCount]);
	}
	argv[argCount] = NULL;
	va_end(args);
	lprintDirectf(LogDebug, "\n");

	int pipefd[2];
	errno = 0;
	if (pipe(pipefd) != 0) {
		lprintf(LogError, "Failed to create pipe for Open vSwitch command: %s\n", strerror(errno));
		return errno;
	}

	errno = 0;
	pid_t pid = fork();
	if (pid == -1) {
		lprintf(LogError, "Failed to fork to execute Open vSwitch command: %s\n", strerror(errno));
		return errno;
	}
	if (pid == 0) { // Child
		close(pipefd[0]);
		close(STDIN_FILENO);
		errno = 0;
		if (!dup2(pipefd[1], STDOUT_FILENO)) exit(errno);
		errno = 0;
		if (!dup2(pipefd[1], STDERR_FILENO)) exit(errno);
		close(pipefd[1]);

		errno = 0;
		execvp(command, (char**)argv);
		exit(errno);
	}
	close(pipefd[1]);

	int readErr = 0;
	if (output != NULL) {
		char readBuf[4 * 1024];
		while (true) {
			errno = 0;
			ssize_t readLen = read(pipefd[0], readBuf, sizeof(readBuf));
			if (readLen < 1) break;
			flexBufferGrow((void**)output, *outputLen, outputCap, (size_t)readLen, 1);
			flexBufferAppend(*output, outputLen, readBuf, (size_t)readLen, 1);
		}
		readErr = errno;
	}
	// Always wait so that we don't end up with a zombie process

	int status;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != expectStatus) {
		lprintf(LogError, "Open vSwitch command %s reported a failure. Exit code: %d\n", command, WEXITSTATUS(status));
		return 1;
	} else if (readErr != 0) {
		lprintf(LogError, "Error while reading output from Open vSwitch command %s: %s\n", command, strerror(readErr));
		return readErr;
	}
	return 0;
}

static const char* ovsToolVersion(char** output, size_t* outputLen, size_t* outputCap, const char* tool) {
	*outputLen = 0;
	int res = ovsCommand(output, outputLen, outputCap, 0, tool, "--version", NULL);
	if (res != 0 || *outputLen < 1) return NULL;

	// Take the last token (w.r.t. spaces) on the first line
	char* p = strchr(*output, '\n');
	if (p == NULL) return NULL;
	*p = '\0';
	while (p >= *output && *p != ' ') --p;
	return p+1;
}

char* ovsVersion(void) {
	char* output;
	size_t outputLen, outputCap;
	flexBufferInit((void**)&output, &outputLen, &outputCap);

	const char* neededCommands[] = {
		"ovsdb-tool",
		"ovsdb-server",
		"ovs-vsctl",
		"ovs-vswitchd",
		"ovs-appctl",
		"ovs-ofctl",
		NULL,
	};

	char* version = NULL;
	for (const char** cmd = neededCommands; *cmd != NULL; ++cmd) {
		const char* cmdVer = ovsToolVersion(&output, &outputLen, &outputCap, *cmd);
		if (cmdVer == NULL) return NULL;
		if (version == NULL) {
			version = strdup(cmdVer);
		} else if (strcmp(cmdVer, version) != 0) {
			return NULL;
		}
	}

	flexBufferFree((void**)&output, &outputLen, &outputCap);
	return version;
}

ovsContext* ovsStart(netContext* net, const char* directory, const char* ovsSchema, int* err) {
	lprintf(LogDebug, "Starting Open vSwitch instance in namespace %p with state directory %s\n", net, directory);

	int localErr;
	if (err == NULL) err = &localErr;

	errno = 0;
	if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
		lprintf(LogError, "Could not create the Open vSwitch state directory '%s': %s\n", directory, strerror(errno));
		*err = errno;
		return NULL;
	}

	errno = 0;
	if (chdir(directory) != 0) {
		lprintf(LogError, "Could not chdir to Open vSwitch state directory '%s': %s\n", directory, strerror(errno));
		*err = errno;
		return NULL;
	}

	ovsContext* ctx = NULL;

	netSwitchNamespace(net);

	// Construct file paths / arguments
	char *dbFile;
	char *ovsdbLogArg, *ovsdbPidArg, *ovsdbSocket, *ovsdbSocketArg, *ovsdbSocketConnArg, *ovsdbControlArg;
	char *ovsLogArg, *ovsPidArg, *ovsControlArg;
	newSprintf(&dbFile, "%s/ovs.db", directory);
	newSprintf(&ovsdbLogArg, "--log-file=%s/ovsdb-server.log", directory);
	newSprintf(&ovsdbPidArg, "--pidfile=%s/ovsdb-server.pid", directory);
	newSprintf(&ovsdbSocket, "%s/ovsdb-server.sock", directory);
	newSprintf(&ovsdbSocketArg, "--remote=punix:%s", ovsdbSocket);
	newSprintf(&ovsdbSocketConnArg, "--db=unix:%s", ovsdbSocket);
	newSprintf(&ovsdbControlArg, "--unixctl=%s/" OVSDB_CTL_FILE, directory);
	newSprintf(&ovsLogArg, "--log-file=%s/ovs-vswitchd.log", directory);
	newSprintf(&ovsPidArg, "--pidfile=%s/ovs-vswitchd.pid", directory);
	newSprintf(&ovsControlArg, "--unixctl=%s/" OVS_CTL_FILE, directory);

	// First, set up the OVSDB daemon. This daemon provides access to the
	// database file that is used to store switch data and manage the other
	// components.

	errno = 0;
	if (unlink(dbFile) != 0 && errno != ENOENT) {
		lprintf(LogError, "Could not delete Open vSwitch database file '%s': %s\n", dbFile, strerror(errno));
		goto abort;
	}

	if (ovsSchema == NULL) ovsSchema = OVS_DEFAULT_SCHEMA_PATH;
	*err = ovsCommand(NULL, NULL, NULL, 0, "ovsdb-tool", "create", dbFile, ovsSchema, NULL);
	if (*err != 0) goto abort;

	*err = ovsCommand(NULL, NULL, NULL, 0, "ovsdb-server", dbFile, "-vconsole:off", "-vsyslog:err", "-vfile:info", "--no-chdir", "--detach", "--monitor", ovsdbLogArg, ovsdbPidArg, ovsdbSocketArg, ovsdbControlArg, NULL);
	if (*err != 0) goto abort;

	*err = ovsCommand(NULL, NULL, NULL, 0, "ovs-vsctl", ovsdbSocketConnArg, "--no-wait", "init", NULL);
	if (*err != 0) goto abort;

	// Next, set up the vswitchd daemon. This daemon manages the virtual
	// switches and their flows.

	*err = ovsCommand(NULL, NULL, NULL, 0, "ovs-vswitchd", &ovsdbSocketConnArg[5], "-vconsole:off", "-vsyslog:err", "-vfile:info", "--mlockall", "--no-chdir", "--detach", "--monitor", ovsLogArg, ovsPidArg, ovsControlArg, NULL);
	if (*err != 0) goto abort;

	ctx = emalloc(sizeof(ovsContext));
	ctx->net = net;
	ctx->directory = strdup(directory);
	ctx->dbSocketConnArg = ovsdbSocketConnArg;
	goto cleanup;
abort:
	// We don't free this unless there was an error (it is used in ctx)
	free(ovsdbSocketConnArg);
cleanup:
	free(dbFile);
	free(ovsdbLogArg); free(ovsdbPidArg); free(ovsdbSocket); free(ovsdbSocketArg); free(ovsdbControlArg);
	free(ovsLogArg); free(ovsPidArg); free(ovsControlArg);
	return ctx;
}

int ovsFree(ovsContext* ctx) {
	int err = switchContext(ctx);
	if (err != 0) return err;

	free(ctx->dbSocketConnArg);
	free(ctx->directory);
	free(ctx);
	return 0;
}

int ovsDestroy(const char* directory) {
	char* ovsdbControl;
	char* ovsControl;
	newSprintf(&ovsdbControl, "%s/" OVSDB_CTL_FILE, directory);
	newSprintf(&ovsControl, "%s/" OVS_CTL_FILE, directory);

	int err = 0;
	if (access(ovsControl, F_OK) != -1) {
		lprintf(LogDebug, "Shutting down Open vSwitch instance with control socket '%s'\n", ovsControl);
		err = ovsCommand(NULL, NULL, NULL, 0, "ovs-appctl", "-t", ovsControl, "exit", NULL);
		if (err != 0) {
			lprintf(LogError, "Failed to destroy Open vSwitch instance with control socket '%s'. Shut down the Open vSwitch system manually with ovs-appctl before continuing.\n");
		}
	}
	if (access(ovsdbControl, F_OK) != -1) {
		lprintf(LogDebug, "Shutting down OVSDB instance with control socket '%s'\n", ovsdbControl);
		err = ovsCommand(NULL, NULL, NULL, 0, "ovs-appctl", "-t", ovsdbControl, "exit", NULL);
		if (err != 0) {
			lprintf(LogError, "Failed to destroy OVSDB instance with control socket '%s'. Shut down the Open vSwitch system manually with ovs-appctl before continuing.\n");
		}
	}

	free(ovsdbControl);
	free(ovsControl);
	return err;
}