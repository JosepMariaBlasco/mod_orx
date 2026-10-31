/*
 * apache.rex -- the APACHE command environment of a native program.
 *
 * Copyright 2026 Josep Maria Blasco
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 *
 * Built into oorexx-host (build/apache_src.h). Every host process loads it
 * once, as a package, in its anchor instance; in every phase of a native
 * program, before the program, the host registers the environment, an
 * OoRexxApache (a .CommandHandler, ooRexx 5.3.0) for that phase, so that
 * the program can write
 *
 *     Address APACHE
 *     "GET URI"
 *     "SET NOTE origin" uri
 *
 * The form of a command is  VERB [link...] thing [key] [value] , where the
 * value is the rest of the line, as it is. Verbs and things are not
 * case-sensitive; keys are kept as written. What it reads comes from the
 * snapshot of the request; what it changes goes to the module as an UPDATE
 * (and changes the snapshot, so that a later GET sees it).
 *
 *   GET thing [INTO var]       a field: the variable is named after it (URI)
 *   GET table key [INTO var]   an entry: TABLE.KEY, the key in uppercase, the
 *                              same stem under the singular and the plural
 *                              name; with repeated entries, the last one
 *   SET field value            (CONTENT_LANGUAGES: the whole list)
 *   SET | ADD | UNSET table key [value]
 *   LIST table [key]           "Name: value" lines, or the values of key, to
 *                              WITH OUTPUT if the command has it, or .output
 *   LOG level message          to Apache's error log, at a LogLevel level
 *
 * Links (PREV, NEXT, MAIN), before the thing, go to a related request, and
 * name the variable: "GET PREV URI" sets PREV_URI. They are read-only.
 *
 * RC: 0 done; 1 there is no such entry or related request (the variable is
 * dropped). ERROR, with RC 2, if the value is one Apache would not take.
 * SYNTAX 93.900, at the command, for a misuse: an unknown verb or thing, or
 * a change the phase does not allow.
 */

return

::routine oorx_field   external "LIBRARY OORXHOST oorx_field"
::routine oorx_entries external "LIBRARY OORXHOST oorx_entries"
::routine oorx_table   external "LIBRARY OORXHOST oorx_table"
::routine oorx_update  external "LIBRARY OORXHOST oorx_update"
::routine oorx_logl    external "LIBRARY OORXHOST oorx_logl"

::class OoRexxApache public inherit CommandHandler

-- One environment per phase: the tables are the class's, built once.
::method init
  expose phase fields ops tables plurals writable levels
  use strict arg number
  phase = self~class~phases[number]
  if phase == .nil then phase = number
  fields = self~class~fields; ops = self~class~ops; tables = self~class~tables
  plurals = self~class~plurals; writable = self~class~writable
  levels = self~class~levels

::attribute phases   class get
::attribute fields   class get
::attribute ops      class get
::attribute tables   class get
::attribute plurals  class get
::attribute writable class get
::attribute levels   class get

::method activate class
  expose phases fields ops tables plurals writable levels
  -- The phases, by their number in oorexx_protocol.h.
  phases = .array~of("CONTENT", "AUTHN", "AUTHZ", "LOG", "POSTREADREQUEST", -
                     "TRANS", "HEADERPARSER", "ACCESS", "TYPE", "FIXUP", "AUTHEN")
  -- Fields: the name in the snapshot (r:...), and the operation that sets it.
  fields = .StringTable~new
  ops = .StringTable~new
  do f over ("THE_REQUEST the_request", "REQUEST_METHOD method",          -
             "SERVER_PROTOCOL protocol", "HOSTNAME hostname",             -
             "UNPARSED_URI unparsed_uri", "URI uri U",                    -
             "QUERY_STRING args Q", "REQUEST_FILENAME filename F",        -
             "PATH_INFO path_info P", "HANDLER handler H",                -
             "CONTENT_TYPE content_type T",                               -
             "CONTENT_ENCODING content_encoding E",                       -
             "CONTENT_LANGUAGES content_languages K", "USER user R",      -
             "AUTH_TYPE auth_type", "STATUS status", "BYTES_SENT bytes_sent", -
             "REMOTE_ADDR useragent_ip", "SERVER_NAME server_name",       -
             "SERVER_PORT server_port", "PHASE")
    parse var f name field op
    fields[name] = field
    if op \== "" then ops[name] = op
  end
  -- Tables: the letter in the snapshot, under the singular and the plural.
  tables = .StringTable~new
  plurals = .StringTable~new
  do t over ("HEADER_IN HEADERS_IN i", "HEADER_OUT HEADERS_OUT o", -
             "ERR_HEADER_OUT ERR_HEADERS_OUT x", "ENV ENV e", "NOTE NOTES n")
    parse var t one many letter
    tables[one] = letter; tables[many] = letter
    plurals[one] = many
  end
  -- The phases in which each thing may change. The providers (AUTHN, AUTHZ)
  -- may change ENV and NOTE; nothing changes in LOG.
  hooks = "POSTREADREQUEST TRANS HEADERPARSER ACCESS AUTHEN TYPE FIXUP"
  writable = .StringTable~new
  writable["URI"]               = "POSTREADREQUEST TRANS"
  writable["QUERY_STRING"]      = "POSTREADREQUEST TRANS FIXUP"
  writable["REQUEST_FILENAME"]  = "TRANS FIXUP"
  writable["PATH_INFO"]         = "TRANS FIXUP"
  do f over "HANDLER CONTENT_TYPE CONTENT_ENCODING CONTENT_LANGUAGES"~subWords
    writable[f] = "TYPE FIXUP"
  end
  writable["USER"]              = "AUTHEN"
  writable["HEADER_IN"]         = "POSTREADREQUEST HEADERPARSER"
  writable["HEADER_OUT"]        = hooks
  writable["ERR_HEADER_OUT"]    = hooks
  writable["ENV"]               = hooks "AUTHN AUTHZ"
  writable["NOTE"]              = hooks "AUTHN AUTHZ"
  -- LogLevel's levels, as APLOG_* numbers.
  levels = .StringTable~new
  do l over "EMERG ALERT CRIT ERROR WARN NOTICE INFO DEBUG"~subWords
    levels[l] = levels~items
  end
  do i = 1 to 8
    levels["TRACE"i] = 7 + i
  end

::method command
  expose phase fields ops tables plurals writable levels
  use arg command, caller
  w = command~words
  if w == 0 then return self~misuse(caller, "no command")
  verb = command~word(1)~upper
  if verb == "LOG" then return self~log(command, caller)
  if "GET SET ADD UNSET LIST"~wordPos(verb) == 0 then
    return self~misuse(caller, "unknown command" command~word(1))

  -- Links to related requests, then the thing.
  i = 2; links = .array~new; prefix = ""
  do while i <= w
    if "PREV NEXT MAIN"~wordPos(command~word(i)~upper) == 0 then leave
    links~append(command~word(i)~upper~lower)
    prefix ||= command~word(i)~upper"_"
    i += 1
  end
  if i > w then return self~misuse(caller, verb "of what?")
  thing = command~word(i)~upper
  i += 1
  isField = fields~hasIndex(thing)
  if \isField, \tables~hasIndex(thing) then
    return self~misuse(caller, "unknown field or table" command~word(i - 1))

  -- Changes: only to the request itself, and where the phase allows them.
  if verb \== "GET", verb \== "LIST" then do
    if links~items > 0 then
      return self~misuse(caller, "a related request cannot be changed")
    if \isField then base = self~singular(thing); else base = thing
    allowed = writable[base]
    if allowed == .nil then allowed = ""
    if allowed~wordPos(phase) == 0 then
      return self~misuse(caller, base "cannot be changed in the" phase "phase")
  end

  -- The request the command is about.
  q = 0
  do link over links
    q = oorx_field(q, link)
    if q == "" | q == .nil then return self~missing(caller, verb, prefix, thing, isField)
  end

  if isField then select case verb
    when "GET" then do
      target = self~into(command, i, caller)
      if target == .nil then return 0       -- a misuse, already raised
      if target == "" then target = prefix || thing
      if thing == "PHASE" then value = phase
      else do
        -- In LOG, the final status and bytes: those of the last request.
        if phase == "LOG", links~items == 0, "STATUS BYTES_SENT"~wordPos(thing) > 0 then
          do while oorx_field(q, "next") \== ""
            q = oorx_field(q, "next")
          end
        value = oorx_field(q, fields[thing])
      end
      caller[target] = value
      return 0
    end
    when "SET" then do
      value = self~rest(command, i)
      if thing == "REQUEST_FILENAME", value~left(1) \== "/" then
        return caller~raise("ERROR", 2, "APACHE: REQUEST_FILENAME must be an absolute path:" value)
      if thing == "CONTENT_LANGUAGES" then
        value = value~translate(" ", ",")~space(1)~translate(",", " ")
      if self~control(value) then
        return caller~raise("ERROR", 2, "APACHE: a control character in" thing)
      call oorx_update ops[thing], "", value
      return 0
    end
    otherwise return self~misuse(caller, verb "is for tables, and" thing "is a field")
  end

  -- A table.
  letter = tables[thing]
  if verb == "LIST" then do
    if i < w then return self~misuse(caller, "LIST" thing "takes one key at most")
    key = command~word(i)
    if caller~isRedirected("Output") then out = caller~output
    else out = .output
    if key == "" then do
      t = oorx_table(q, letter)
      do j = 1 to t~items by 2
        out~lineOut(t[j]":" t[j + 1])
      end
    end
    else do v over oorx_entries(q, letter, key)
      out~lineOut(v)
    end
    return 0
  end
  if i > w then return self~misuse(caller, verb thing "needs a key")
  key = command~word(i)
  i += 1
  select case verb
    when "GET" then do
      target = self~into(command, i, caller)
      if target == .nil then return 0
      values = oorx_entries(q, letter, key)
      if target \== "" then do
        if values~items == 0 then do
          caller~dropVar(target); return 1
        end
        caller[target] = values~lastItem
        return 0
      end
      stem = self~stem(caller, prefix, thing)
      if values~items == 0 then do
        stem~remove(key~upper); return 1
      end
      stem[key~upper] = values~lastItem
      return 0
    end
    when "UNSET" then do
      if i <= w then return self~misuse(caller, "UNSET" thing key "takes no value")
      call oorx_update "u"letter, key, ""
      return 0
    end
    otherwise do                        -- SET, ADD
      value = self~rest(command, i)
      if "ioxIOX"~pos(letter) > 0 then do
        if \self~token(key) then
          return caller~raise("ERROR", 2, "APACHE: not a header name:" key)
        if self~control(value) then
          return caller~raise("ERROR", 2, "APACHE: a control character in header" key)
      end
      call oorx_update verb~left(1)~lower || letter, key, value
      return 0
    end
  end

-- LOG level message: the message is the rest of the line.
::method log private
  expose levels
  use arg command, caller
  level = command~word(2)~upper
  if \levels~hasIndex(level) then
    return self~misuse(caller, "LOG needs a level: emerg alert crit error warn" -
                       "notice info debug trace1..trace8")
  call oorx_logl levels[level], self~rest(command, 3)
  return 0

-- The singular name of a table.
::method singular private
  expose plurals
  use arg thing
  do one over plurals~allIndexes
    if plurals[one] == thing then return one
  end
  return thing

-- The stem of a table: the one the program has under either name, or a new
-- one, and then the same under both (ENV has one name).
::method stem private
  expose plurals
  use arg caller, prefix, thing
  one = prefix || self~singular(thing)
  many = prefix || plurals[self~singular(thing)]
  stem = .nil
  if caller~hasVar(one".") then stem = caller[one"."]
  else if caller~hasVar(many".") then stem = caller[many"."]
  if \stem~isA(.Stem) then stem = .Stem~new
  caller[one"."] = stem
  caller[many"."] = stem
  return stem

-- GET found nothing: no value, RC 1.
::method missing private
  use arg caller, verb, prefix, thing, isField
  if verb \== "GET" then return 1
  if isField then caller~dropVar(prefix || thing)
  else self~stem(caller, prefix, thing)
  return 1

-- After GET's thing (and key): nothing, or INTO var. Returns the variable,
-- "" for none, or .nil after raising SYNTAX.
::method into private
  use arg command, i, caller
  w = command~words
  if i > w then return ""
  if i + 1 == w, command~word(i)~upper == "INTO" then do
    var = command~word(i + 1)
    if var~dataType("V"), "0123456789."~pos(var~left(1)) == 0 then return var
    self~misuse(caller, "INTO needs the name of a variable:" var)
    return .nil
  end
  self~misuse(caller, "unexpected:" command~subWord(i))
  return .nil

-- The value: the rest of the line after word i - 1 and one blank, as it is.
::method rest private
  use arg command, i
  if i == 1 then start = 1
  else start = command~wordIndex(i - 1) + command~word(i - 1)~length + 1
  if start > command~length then return ""
  return command~substr(start)

-- A header name: an HTTP token.
::method token private
  use arg key
  return key \== "" & key~verify("!#$%&'*+-.^_`|~0123456789" || -
         "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == 0

-- A control character other than a tab.
::method control private
  use arg value
  return value~verify(xrange("00"x, "08"x) || xrange("0A"x, "1F"x) || "7F"x, "M") > 0

::method misuse private
  use arg caller, message
  return caller~raise("SYNTAX", 93.900, .array~of("APACHE:" message))
