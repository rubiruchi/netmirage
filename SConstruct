import os
import subprocess
import time
import SCons.Errors

# Public version information
appName = 'NetMirage'
appVersionMajor = 0
appVersionMinor = 9
appVersionRevision = 0

def confBuild(name, build, targetName, commonDeps, cflags=None, linkflags=None):
	srcDir = "src/"+name
	buildDir = "build/"+name+"-"+build
	
	env = Environment()
	if cflags is not None:
		for cflag in cflags:
			env.Append(CFLAGS = cflag)
	if linkflags is not None:
		for linkflag in linkflags:
			env.Append(LINKFLAGS = linkflag)
	
	env.Append(CFLAGS = "-I%s"%(Dir(srcDir).get_abspath()))
	
	Export('env')
	Export('targetName')
	Export('commonDeps')
	SConscript(srcDir+"/SConstruct", variant_dir=buildDir, duplicate=0)


# Create automatically generated files (forced on every build)

autoDir = "build/auto"
if not os.path.exists(autoDir):
    os.makedirs(autoDir)

# Set version based on whether we're using git or not
appVersion = '%d.%d.%d'%(appVersionMajor, appVersionMinor, appVersionRevision)
if os.path.isdir('.git'):
	try:
		commitId = subprocess.check_output(['git', 'log', '--format=%h', '-n 1']).strip()
		print 'Detected git install; using commit version'
		appVersion = '%s.%s'%(appVersion, commitId)
		
		dirtyRepo = subprocess.call(['git', 'diff-index', '--cached', 'HEAD', '--quiet'])
		if not dirtyRepo:
			dirtyRepo = subprocess.call(['git', 'diff-files', '--quiet'])
			if not dirtyRepo:
				dirtyRepo = subprocess.check_output(['git', 'ls-files', '--exclude-standard', '--others', '../..']).strip()
		if dirtyRepo:
			print 'Dirty git workspace; using timed version'
			appVersion = '%s.%d'%(appVersion, int(time.time()))
	except:
		pass
print 'Version:', appName, appVersion

# Write version definitions
verFile = File(autoDir+"/version.c")
with open(verFile.get_abspath(), 'w') as handle:
	handle.write("""
		// This file was generated by SConstruct. DO NOT EDIT!
		#include "version.h"
		const char* getVersionString(void) { return "%s %s"; }
		uint16_t getVersionMajor(void) { return %d; }
		uint16_t getVersionMinor(void) { return %d; }
		uint16_t getVersionRevision(void) { return %d; }
	""" % (appName, appVersion, appVersionMajor, appVersionMinor, appVersionRevision))
commonDeps = [verFile];

# Common build flags
debugMode = int(ARGUMENTS.get('debug', 0))
commonCFlags = []
commonLinkFlags = []
commonCFlags += ['-Wall -Wextra -Wundef -Wendif-labels -Wshadow -Wpointer-arith -Wbad-function-cast -Wcast-qual -Wcast-align -Wwrite-strings -Wconversion -Waggregate-return -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes -Wmissing-declarations -Wpacked -Wredundant-decls -Wnested-externs -Winline -Winvalid-pch -Wdisabled-optimization']
commonCFlags += ['-Wno-unused-parameter -Wno-missing-field-initializers']
commonCFlags += ['-Werror -fmax-errors=10 -std=c99']
if debugMode:
	commonCFlags += ['-g3 -DDEBUG']
	build = "debug"
	targetSuffix = "-debug"
else:
	commonCFlags += ['-O3 -flto -fno-fat-lto-objects']
	commonLinkFlags += ['-O3 -flto']
	build = "release"
	targetSuffix = ""

# Resolve glib dependency
try:
	glibFlags = subprocess.check_output(['pkg-config', '--cflags', 'glib-2.0']).strip()
	glibLibs = subprocess.check_output(['pkg-config', '--libs', 'glib-2.0']).strip()
except:
	raise SCons.Errors.UserError, 'Could not locate GLib (GNOME Library) 2.0 install location. Ensure that glib is installed and visible to pkg-config.'
print 'Using glib 2.0 [flags = %s] [libs = %s]'%(glibFlags, glibLibs)
commonCFlags += [glibFlags]
commonLinkFlags += [glibLibs]

# Configure possible build targets
confBuild('netmirage', build, 'netmirage'+targetSuffix, commonDeps, commonCFlags, commonLinkFlags)
