/*******************************************************************************
 * Copyright © 2018 Nik Unger, Ian Goldberg, Qatar University, and the Qatar
 * Foundation for Education, Science and Community Development.
 *
 * This file is part of NetMirage.
 *
 * NetMirage is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * NetMirage is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with NetMirage. If not, see <http://www.gnu.org/licenses/>.
 *******************************************************************************/
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argp.h>
#include <glib.h>
#include <libxml/parser.h>
#include <strings.h>

#include "app.h"
#include "ip.h"
#include "log.h"
#include "mem.h"
#include "setup.h"
#include "version.h"

// TODO: normalize naming conventions for "client", "root", etc.
// TODO: more specific error codes than 1
// TODO: normalize (return result, out err) vs (return err, out result)
// TODO: normalize res and err

// Argument data recovered by argp
static struct {
	size_t edgeNodeCap; // Buffer length is stored in the setupParams
	bool loadedEdgesFromSetup;

	// Actual parameters for setup procedure
	setupParams params;
	setupGraphMLParams gmlParams;
} args;

enum {
	AcOvsDir = 256,
	AcOvsSchema,
	AcClientNode,
} ArgCodes;

// Divisors for GraphML bandwidths
static const float ShadowDivisor = 125.f;    // KiB/s
static const float ModelNetDivisor = 1000.f; // Kb/s

#define DEFAULT_CLIENTS_SUBNET "10.0.0.0/8"

#define DEFAULT_OVS_DIR    "/tmp/netmirage"

// Adds an edge node based on strings, which may be NULL
static bool addEdgeNode(const char* ipStr, const char* intfStr, const char* macStr, const char* vsubnetStr, const char* remoteDev, const char* remoteApps) {
	edgeNodeParams params;
	if (!ip4GetAddr(ipStr, &params.ip)) return false;

	if (intfStr != NULL && *intfStr == '\0') return false;
	if (intfStr == NULL) {
		params.intf = NULL;
	} else {
		params.intf = eamalloc(strlen(intfStr), 1, 1); // Extra byte for NUL
		strcpy(params.intf, intfStr);
	}

	params.macSpecified = (macStr != NULL);
	if (params.macSpecified) {
		if (!macGetAddr(macStr, &params.mac)) return false;
	}

	params.vsubnetSpecified = (vsubnetStr != NULL);
	if (params.vsubnetSpecified) {
		if (!ip4GetSubnet(vsubnetStr, &params.vsubnet)) return false;
	}
	if (remoteDev == NULL) {
		params.remoteDev = NULL;
	} else {
		params.remoteDev = eamalloc(strlen(remoteDev), 1, 1);
		strcpy(params.remoteDev, remoteDev);
	}
	params.remoteApps = 0;
	if (remoteApps != NULL) {
		sscanf(remoteApps, "%" SCNu32, &params.remoteApps);
	}
	flexBufferGrow((void**)&args.params.edgeNodes, args.params.edgeNodeCount, &args.edgeNodeCap, 1, sizeof(edgeNodeParams));
	flexBufferAppend(args.params.edgeNodes, &args.params.edgeNodeCount, &params, 1, sizeof(edgeNodeParams));
	return true;
}

static error_t parseArg(int key, char* arg, struct argp_state* state, unsigned int argNum) {
	switch (key) {
	case 'd': args.params.destroyOnly = true; break;
	case 'k': args.params.keepOldNetworks = true; break;
	case 'f': args.params.srcFile = arg; break;
	case AcOvsDir: args.params.ovsDir = arg; break;
	case AcOvsSchema: args.params.ovsSchema = arg; break;

	case 'i': {
		args.params.edgeNodeDefaults.intfSpecified = true;
		args.params.edgeNodeDefaults.intf = arg;
		break;
	}
	case 'n': {
		if (!ip4GetSubnet(arg, &args.params.edgeNodeDefaults.globalVSubnet)) {
			fprintf(stderr, "Invalid global virtual client subnet specified: '%s'\n", arg);
			return EINVAL;
		}
		break;
	}
	case 'e': {
		// We ignore edge configuration in the setup file's [emulator] group
		if (state == NULL) break;

		// If we just found an explicit edge node for the first time after
		// loading configuration from the setup file, erase the old edges
		if (args.loadedEdgesFromSetup) {
			args.params.edgeNodeCount = 0;
			args.loadedEdgesFromSetup = false;
		}

		char* ip = arg;
		char* intf = NULL;
		char* mac = NULL;
		char* vsubnet = NULL;
		char* rdev = NULL;
		char* rapps = NULL;

		char* optionSep = arg;
		while (true) {
			optionSep = strchr(optionSep, ',');
			if (optionSep == NULL) break;

			*optionSep = '\0';
			++optionSep;

			// If the '=' was found in a subsequent option, that's ok because
			// none of the valid options include ',' in their name; we will err
			char* keyValSep = strchr(optionSep, '=');
			if (keyValSep == NULL) {
				fprintf(stderr, "Invalid format for edge node argument '%s'\n", arg);
				return EINVAL;
			}
			size_t cmpLen = (size_t)(keyValSep - optionSep);
			if (cmpLen == 0) {
				fprintf(stderr, "Empty option name in edge node argument '%s'\n", arg);
				return EINVAL;
			}

			if (strncmp(optionSep, "iface", cmpLen) == 0) {
				intf = keyValSep+1;
			} else if (strncmp(optionSep, "mac", cmpLen) == 0) {
				mac = keyValSep+1;
			} else if (strncmp(optionSep, "vsubnet", cmpLen) == 0) {
				vsubnet = keyValSep+1;
			} else if (strncmp(optionSep, "rdev", cmpLen) == 0) {
				rdev = keyValSep+1;
			} else if (strncmp(optionSep, "rapps", cmpLen) == 0) {
				rapps = keyValSep+1;
			} else {
				*keyValSep = '\0';
				fprintf(stderr, "Unknown option '%s' in edge node argument '%s'\n", optionSep, arg);
				return EINVAL;
			}
		}

		if (!addEdgeNode(ip, intf, mac, vsubnet, rdev, rapps)) {
			fprintf(stderr, "Edge node argument '%s' was invalid\n", arg);
			return EINVAL;
		}
		break;
	}

	case 'I':
		if (!ip4GetAddr(arg, &args.params.routingIp)) {
			fprintf(stderr, "Invalid routing IP address specified: '%s'\n", arg);
			return EINVAL;
		}
		break;
	case 'E': args.params.edgeFile = arg; break;
	case 'q': args.params.quiet = true; break;

	case 'p': args.params.nsPrefix = arg; break;

	case 'r': {
		const char* options[] = {"custom", "init", NULL};
		bool settings[] = {false, true};
		long index = matchArg(arg, options);
		if (index < 0) {
			fprintf(stderr, "Unknown root namespace location '%s'\n", arg);
			return EINVAL;
		}
		args.params.rootIsInitNs = settings[index];
		break;
	}

	case 'm': args.params.softMemCap = (size_t)(1024.0 * 1024.0 * strtod(arg, NULL)); break;

	case 'u': {
		const char* options[] = {"shadow", "modelnet", "KiB", "Kb", NULL};
		float divisors[] = {ShadowDivisor, ModelNetDivisor, ShadowDivisor, ModelNetDivisor};
		long index = matchArg(arg, options);
		if (index < 0) {
			fprintf(stderr, "Unknown bandwidth units '%s'\n", arg);
			return EINVAL;
		}
		args.gmlParams.bandwidthDivisor = divisors[index];
		break;
	}
	case 'w': args.gmlParams.weightKey = arg; break;
	case AcClientNode: args.gmlParams.clientType = arg; break;
	case '2': args.gmlParams.twoPass = true; break;

	default: return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

// Configures edge nodes using information in the setup file
static bool readSetupEdges(GKeyFile* file) {
	gchar** groups = g_key_file_get_groups(file, NULL);
	for (gchar** g = groups; *g != NULL; ++g) {
		gchar* group = *g;
		// We allow "node" for backwards compatibility, but do not advertise it
		if (strncmp(group, "edge", 4) == 0 || strncmp(group, "node", 4) == 0) {
			char* ip = g_key_file_get_string(file, group, "ip", NULL);
			char* intf = g_key_file_get_string(file, group, "iface", NULL);
			char* mac = g_key_file_get_string(file, group, "mac", NULL);
			char* vsubnet = g_key_file_get_string(file, group, "vsubnet", NULL);
			char* rdev = g_key_file_get_string(file, group, "rdev", NULL);
			char* rapps = g_key_file_get_string(file, group, "rapps", NULL);
			bool added = addEdgeNode(ip, intf, mac, vsubnet, rdev, rapps);

			g_free(ip);
			g_free(intf);
			g_free(mac);
			g_free(vsubnet);
			g_free(rdev);
			g_free(rapps);

			if (!added) {
				fprintf(stderr, "In setup file: invalid configuration for edge node '%s'\n", group);
				return false;
			}
			args.loadedEdgesFromSetup = true;
		}
	}
	g_strfreev(groups);
	return true;
}

int main(int argc, char** argv) {
	appInit("NetMirage Core", getVersion());

	// Launch worker processes so that we can drop our privileges as quickly as
	// possible (note that we have not handled any user input at this point)
	if (setupInit() != 0) {
		lprintln(LogError, "Failed to start worker processes. Elevation may be required.");
		logCleanup();
		return 1;
	}

	// Initialize libxml and ensure that the shared object is correct version
	LIBXML_TEST_VERSION

	// Command-line switch definitions
	struct argp_option generalOptions[] = {
			{ "destroy",      'd', NULL,   OPTION_ARG_OPTIONAL, "If specified, any previous virtual network created by the program will be destroyed and the program terminates without creating a new network.", 0 },
			{ "keep",         'k', NULL,   OPTION_ARG_OPTIONAL, "If specified, previous virtual networks created by the program are not destroyed before setting up new ones. Note that --destroy takes priority.", 0 },
			{ "file",         'f', "FILE", 0,                   "The GraphML file containing the network topology. If omitted, the topology is read from stdin.", 0 },
			{ "setup-file",   's', "FILE", 0,                   "The file containing setup information about edge nodes and emulator interfaces. This file is a key-value file (similar to an .ini file). Every group whose name begins with \"edge\" or \"node\" denotes the configuration for an edge node. The keys and values permitted in an edge node group are the same as those in an --edge-node argument. There may also be an \"emulator\" group. This group may contain any of the long names for command arguments. Note that any file paths specified in the setup file are relative to the current working directory (not the file location). Any arguments passed on the command line override the defaults and those set in the setup file. By default, the program attempts to read setup information from " DEFAULT_SETUP_FILE ".", 0 },

			{ "iface",        'i', "DEVNAME",                                                                  0, "Default interface connected to the edge nodes. Individual edge nodes can override this setting in the setup file or as part of the --edge-nodes argument.", 1 },
			{ "vsubnet",      'n', "CIDR",                                                                     0, "The global subnet to which all virtual clients belong. By default, each edge node is given a fragment of this global subnet in which to spawn clients. Subnets for edge nodes can also be manually assigned rather than drawing them from this larger space. The default value is " DEFAULT_CLIENTS_SUBNET ".", 1 },
			{ "edge-node",    'e', "IP[,iface=DEVNAME][,mac=MAC][,vsubnet=CIDR][,rdev=DEVNAME][,rapps=COUNT]", 0, "Adds an edge node to the configuration. The presence of an --edge-node argument causes all edge node configuration in the setup file to be ignored. The node's IPv4 address must be specified. If the optional \"iface\" portion is specified, it lists the interface connected to the edge node (if omitted, --iface is used). \"mac\" specifies the MAC address of the node (if omitted, it is found using ARP). \"vsubnet\" specifies the subnet, in CIDR notation, for clients in the edge node (if omitted, a subnet is assigned automatically from the --vsubnet range). \"rdev\" refers to the interface on the remote machine that is connected to this machine; this is only used when producing edge node commands using --edge-output. Similarly, \"rapps\" specifies the number of remote applications to configure in the edge node commands.", 1 },

			{ "routing-ip",   'I', "IP",   0,                   "The IP address that edge nodes should use to communicate with the core. This value is only used for generating edge node commands with --edge-output.", 2 },
			{ "edge-output",  'E', "FILE", 0,                   "If specified, commands for instantiating the edge nodes are written to the given file instead of stdout. These commands should be executed on the edge nodes to connect them with the core.", 2 },
			{ "quiet",        'q', NULL,   OPTION_ARG_OPTIONAL, "If specified, no edge information is written to stdout.", 2 },

			{ "verbosity",    'v', "{debug,info,warning,error}", 0, "Verbosity of log output (default: warning).", 3 },
			{ "log-file",     'l', "FILE",                       0, "Log output to FILE instead of stderr. Note: configuration errors will still be written to stderr.", 3 },

			{ "netns-prefix", 'p',         "PREFIX",         0, "Prefix string for network namespace files, which are visible to \"ip netns\" (default: \"nm-\").", 4 },
			{ "root-ns",      'r',         "{custom,init}",  0, "Specifies the location of the \"root\" namespace, which is used for routing traffic between external interfaces and the internal network. \"custom\" places the links in a custom namespace. \"init\" places the links in the same namespace as the init process. This may be necessary if your edges are connected to advanced interfaces that cannot be moved. However, using the init namespace as the root may cause some global networking settings to be modified. Default: \"custom\".", 4 },
			{ "ovs-dir",      AcOvsDir,    "DIR",            0, "Directory for storing temporary Open vSwitch files, such as the flow database and management sockets (default: \"" DEFAULT_OVS_DIR "\").", 4 },
			{ "ovs-schema",   AcOvsSchema, "FILE",           0, "Path to the OVSDB schema definition for Open vSwitch (default: \"/usr/share/openvswitch/vswitch.ovsschema\").", 4 },

			{ "mem",          'm', "MiB",    0, "Approximate maximum memory use, specified in MiB. The program may use more than this amount if needed.", 5 },

			// File-specific options get priorities [50 - 99]

			{ NULL },
	};
	struct argp_option gmlOptions[] = {
			{ "units",        'u',          "{shadow,modelnet,KiB,Kb}", 0,                   "Specifies the bandwidth units used in the input file. Shadow uses KiB/s (the default), whereas ModelNet uses Kbit/s." },
			{ "weight",       'w',          "KEY",                      0,                   "Edge parameter to use for computing shortest paths for static routes. Must be a key used in the GraphML file (default: \"latency\")." },
			{ "client-node",  AcClientNode, "TYPE",                     0,                   "Type of client nodes. Nodes in the GraphML file whose \"type\" attribute matches this value will be clients. If omitted, all nodes are clients." },
			{ "two-pass",     '2',          NULL,                       OPTION_ARG_OPTIONAL, "This option must be specified if the GraphML file does not place all <node> tags before all <edge> tags. This option doubles the data retrieved from disk." },
			{ NULL },
	};
	struct argp_option defaultDoc[] = { { "\n These options provide program documentation:", 0, NULL, OPTION_DOC | OPTION_NO_USAGE }, { NULL } };

	struct argp gmlArgp = { gmlOptions, &appParseArg };
	struct argp defaultDocArgp = { defaultDoc };

	struct argp_child children[] = {
			{ &gmlArgp, 0, "These options apply specifically to GraphML files:\n", 50 },
			{ &defaultDocArgp, 0, NULL, 100 },
			{ NULL },
	};
	struct argp argp = { generalOptions, &appParseArg, NULL, "Sets up virtual networking infrastructure for a NetMirage core node.", children };

	// Default arguments
	args.params.nsPrefix = "nm-";
	args.params.ovsDir = DEFAULT_OVS_DIR;
	args.params.softMemCap = 2LL * 1024LL * 1024LL * 1024LL;
	args.params.destroyOnly = false;
	args.params.keepOldNetworks = false;
	args.params.quiet = false;
	args.params.rootIsInitNs = false;
	ip4GetSubnet(DEFAULT_CLIENTS_SUBNET, &args.params.edgeNodeDefaults.globalVSubnet);
	args.gmlParams.bandwidthDivisor = ShadowDivisor;
	args.gmlParams.weightKey = "latency";
	args.gmlParams.twoPass = false;

	int err = 0;

	err = appParseArgs(&parseArg, &readSetupEdges, &argp, "emulator", NULL, 's', 'l', 'v', argc, argv);
	if (err != 0) goto cleanup;

	lprintf(LogInfo, "Starting NetMirage Core %s\n", getVersion());

	lprintln(LogInfo, "Loading edge node configuration");
	err = setupConfigure(&args.params);
	if (err != 0) goto cleanup;

	if (!args.params.destroyOnly) {
		lprintln(LogInfo, "Beginning network construction");
		err = setupGraphML(&args.gmlParams);
	}

	if (err != 0) {
		lprintf(LogError, "A fatal error occurred: code %d\n", err);
		lprintln(LogWarning, "Attempting to destroy partially-constructed network");
		destroyNetwork();
	} else {
		lprintln(LogInfo, "All operations completed successfully");
	}

cleanup:
	setupCleanup();
	if (args.params.edgeNodes != NULL) {
		for (size_t i = 0; i < args.params.edgeNodeCount; ++i) {
			edgeNodeParams* edge = &args.params.edgeNodes[i];
			if (edge->intf != NULL) free(edge->intf);
			if (edge->remoteDev != NULL) free(edge->remoteDev);
		}
	}
	flexBufferFree((void**)&args.params.edgeNodes, &args.params.edgeNodeCount, &args.edgeNodeCap);
	xmlCleanupParser();
	appCleanup();

	return err;
}
