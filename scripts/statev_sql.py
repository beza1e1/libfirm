#! /usr/bin/env python

import sys
import os
import re
import time
import stat
import fileinput
import tempfile
import optparse

class DummyFilter:
	def match(self, dummy):
		return True

class EmitBase:
	ctx_field_ids = {}
	ev_field_ids = {}
	types = {}

	def create_table(self, cols, name, defaulttype, keytype, extra=""):
		c  = "create table if not exists `%s` (\n" % name
		c += "\t`id` %s\n" % keytype

		for x in cols:
			name = x
			type = self.types[defaulttype]
			if x[0] == '$':
				name = x[1:]
				type = self.types["text"]
			elif x[0] == '?':
				name = x[1:]
				type = self.types["bool"]

			c += "\t,`%s` %s\n" % (name, type)
		c += extra
		c += ");"
		self.execute(c)

# Abstraction for mysql sql connection and sql syntax
class EmitMysql(EmitBase):
	tmpfile_mode = stat.S_IREAD | stat.S_IROTH | stat.S_IWUSR

	def execute(self, query, *args):
		#print query + " %s\n" % str(tuple(args))
		self.cursor.execute(query, *args);
		self.conn.commit()

	def connect(self, options):
		import MySQLdb

		args = dict()
		if options.password:
			args['passwd'] = options.password
		if not options.host:
			options.host = 'localhost'
		args['user'] = options.user
		args['host'] = options.host
		args['db']   = options.database

		self.conn     = MySQLdb.connect(**args)
		self.options  = options
		self.cursor   = self.conn.cursor()

	def __init__(self, options, ctxcols, evcols):
		self.connect(options)

		self.types["text"] = "varchar(80) default null";
		self.types["data"] = "double default null";
		self.types["bool"] = "bool";

		self.ctxtab = options.prefix + "ctx"
		self.evtab  = options.prefix + "ev"

		if not options.update:
			self.execute('drop table if exists `%s`' % self.evtab)
			self.execute('drop table if exists `%s`' % self.ctxtab)

		self.create_table(ctxcols, self.ctxtab, "text", "int auto_increment", extra = ", PRIMARY KEY (`id`)")
		self.create_table(evcols, self.evtab, "data", "int not null", extra = ", INDEX(`id`)")

		keys  = "id, " + ", ".join(evcols)
		marks = ",".join(['%s'] * (len(evcols)+1))
		self.evinsert = "insert into `%s` values (%s)" % (self.evtab, marks)

		keys  = ", ".join(ctxcols)
		marks = ",".join(['%s'] * len(ctxcols))
		self.ctxinsert = "insert into `%s` (%s) values (%s)" % (self.ctxtab, keys, marks)

	def ev(self, curr_id, evitems):
		self.execute(self.evinsert, (curr_id,) + tuple(evitems))

	def ctx(self, ctxitems):
		self.execute(self.ctxinsert, tuple(ctxitems))
		self.conn.commit()
		id = self.cursor.lastrowid
		return id

	def commit(self):
		self.conn.commit()

# Abstraction for sqlite3 databases and sql syntax
class EmitSqlite3(EmitBase):
	def execute(self, query, *args):
		#print query + " %s\n" % str(tuple(args))
		self.cursor.execute(query, *args)

	def __init__(self, options, ctxcols, evcols):
		import sqlite3

		if options.database == None:
			print "Have to specify database (file-)name for sqlite"
			sys.exit(1)

		if not options.update:
			if os.path.isfile(options.database):
				os.unlink(options.database)

		self.conn = sqlite3.connect(options.database)
		self.cursor = self.conn.cursor()

		self.types["data"] = "double"
		self.types["text"] = "text"
		self.types["bool"] = "int"
		self.ctxtab = options.prefix + "ctx"
		self.evtab  = options.prefix + "ev"

		self.create_table(ctxcols, self.ctxtab, "text", "integer primary key")
		self.create_table(evcols, self.evtab, "data", "int")
		self.execute("CREATE INDEX IF NOT EXISTS `%sindex` ON `%s`(id)"
				% (self.evtab, self.evtab))

		keys  = "id, " + ", ".join(evcols)
		marks = ",".join(["?"] * (len(evcols)+1))
		self.evinsert = "insert into `%s` values (%s)" % (self.evtab, marks)

		keys  = ", ".join(ctxcols)
		marks = ",".join(["?"] * len(ctxcols))
		self.ctxinsert = "insert into `%s` (%s) values (%s)" % (self.ctxtab, keys, marks)

		self.nextid = 0

	def ev(self, curr_id, evitems):
		self.execute(self.evinsert, (curr_id,) + tuple(evitems))

	def ctx(self, ctxitems):
		curr_id = self.nextid
		self.nextid += 1
		self.execute(self.ctxinsert, tuple(ctxitems))
		self.conn.commit()
		return self.cursor.lastrowid

	def commit(self):
		self.conn.commit()

class Conv:
	engines = { 'sqlite3': EmitSqlite3, 'mysql': EmitMysql }

	# Pass that determines event and context types
	def find_heads(self):
		n_ev    = 0
		ctxind  = 0
		evind   = 0
		ctxcols = dict()
		evcols  = dict()
		linenr  = 0

		self.valid_keys = set()

		inp = self.input()

		for line in inp:
			linenr += 1
			fields  = line.strip().split(";")
			if fields[0] == 'P':
				if (len(fields)-1) % 2 != 0:
					print "%s: Invalid number of fields after 'P'" % linenr

				for i in range(1,len(fields),2):
					key = fields[i]
					if not ctxcols.has_key(key):
						ctxcols[key] = ctxind
						ctxind += 1

			elif fields[0] == 'E':
				if (len(fields)-1) % 2 != 0:
					print "%s: Invalid number of fields after 'E'" % linenr

				self.n_events += 1
				for i in range(1,len(fields),2):
					key = fields[i]
					if not self.filter.match(key):
						continue

					if not evcols.has_key(key):
						self.valid_keys.add(key)
						evcols[key] = evind
						evind += 1

		self.ctxcols = ctxcols
		self.evcols = evcols
		return (ctxcols, evcols)

	def input(self):
		return fileinput.FileInput(files=self.files, openhook=fileinput.hook_compressed)

	def flush_events(self, id):
		isnull = True
		for e in self.evvals:
			if e != None:
				isnull = False
				break
		if isnull:
			return

		self.emit.ev(id, self.evvals)
		self.evvals = [None] * len(self.evvals)

	def flush_ctx(self):
		if not self.pushpending:
			return
		self.pushpending = False
		self.curr_id = self.emit.ctx(self.ctxvals)

	def fill_tables(self):
		lineno     = 0
		ids        = 0
		self.curr_id = -1
		keystack   = []
		idstack    = []
		curr_event = 0
		last_prec  = -1
		self.pushpending = False
		self.ctxvals = [None] * len(self.ctxcols)
		self.evvals  = [None] * len(self.evcols)

		for line in self.input():
			lineno += 1
			items   = line.strip().split(';')
			op      = items[0]

			# Push context command
			if op == 'P':
				self.flush_events(self.curr_id)

				# push the keys
				for p in range(1,len(items),2):
					key = items[p]
					val = items[p+1]

					keystack.append(key)
					idstack.append(self.curr_id)

					keyidx = self.ctxcols[key]
					assert self.ctxvals[keyidx] == None
					self.ctxvals[keyidx] = val
				self.pushpending = True

			# Pop context command
			elif op == 'O':

				# For now... we could optimize this
				self.flush_ctx()

				# We process fields in reverse order to makes O's match the
				# order of previous P's
				for p in range(len(items)-1,0,-1):
					self.flush_events(self.curr_id)

					popkey  = items[p]
					key     = keystack.pop()
					self.curr_id = idstack.pop()

					if popkey != key:
						print "unmatched pop in line %d, push key %s, pop key: %s" % (lineno, key, popkey)

					keyidx = self.ctxcols[key]
					assert self.ctxvals[keyidx] != None
					self.ctxvals[keyidx] = None

			elif op == 'E':
				curr_event += 1

				self.flush_ctx()

				# Show that we make progress
				if self.verbose:
					prec = curr_event * 10 / self.n_events
					if prec > last_prec:
						last_prec = prec
						print '%10d / %10d' % (curr_event, self.n_events)

				for p in range(1,len(items),2):
					key   = items[p]
					if key not in self.evcols:
						continue

					keyidx = self.evcols[key]
					if self.evvals[keyidx] != None:
						self.flush_events(self.curr_id)

					value          = items[p+1]
					self.evvals[keyidx] = value

	def __init__(self):
		parser = optparse.OptionParser('usage: %prog [options]  <event file...>', add_help_option=False)
		parser.add_option("", "--help",                        help="show this help message and exit", action="help")
		parser.add_option("", "--update",     dest="update",   help="update database instead of dropping all existing values", action="store_true", default=False)
		parser.add_option("-v", "--verbose",  dest="verbose",  help="verbose messages",         action="store_true", default=False)
		parser.add_option("-f", "--filter",   dest="filter",   help="regexp to filter event keys", metavar="REGEXP")
		parser.add_option("-u", "--user",     dest="user",     help="user",               metavar="USER")
		parser.add_option("-h", "--host",     dest="host",     help="host",               metavar="HOST")
		parser.add_option("-p", "--password", dest="password", help="password",           metavar="PASSWORD")
		parser.add_option("-D", "--database", dest="database", help="database",           metavar="DB")
		parser.add_option("-e", "--engine",   dest="engine",   help="engine (sqlite3, mysql)",             metavar="ENG", default='sqlite3')
		parser.add_option("-P", "--prefix",   dest="prefix",   help="table prefix",       metavar="PREFIX", default='')
		(options, args) = parser.parse_args()

		self.n_events = 0
		self.stmts    = dict()
		self.verbose  = options.verbose

		if len(args) < 1:
			parser.print_help()
			sys.exit(1)

		self.files  = []
		files       = args

		for file in files:
			if not os.path.isfile(file):
				print "cannot find input file %s" % (file, )
			else:
				self.files.append(file)

		if len(self.files) < 1:
			print "no input file to process"
			sys.exit(3)

		if options.filter:
			self.filter = re.compile(options.filter)
		else:
			self.filter = DummyFilter()

		if options.engine in self.engines:
			engine = self.engines[options.engine]
		else:
			print 'engine %s not found' % options.engine
			print 'we offer: %s' % self.engines.keys()
			sys.exit(0)

		if options.verbose:
			print "determining schema..."

		(ctxcols, evcols) = self.find_heads()
		if options.verbose:
			print "context schema:"
			print ctxcols
			print "event schema:"
			print evcols

		self.emit = engine(options, ctxcols, evcols)

		if options.verbose:
			print "filling tables..."
		self.fill_tables()
		if options.verbose:
			print "comitting..."
		self.emit.commit()

if __name__ == "__main__":
	Conv()
