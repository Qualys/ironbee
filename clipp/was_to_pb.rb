#!/usr/bin/env ruby

# Reads Qualys WAS XML files and produces CLIPP PB files based on them.
# The resulting inputs are barebones as the XML contains only portions of the
# needed data.  It is strongly suggested to make use of @addmissing and
# @fillbody modifiers.

require 'rexml/document'
require 'base64'
$:.unshift(File.expand_path(File.dirname(__FILE__)))
require 'clippscript'
require 'hash_to_pb'

def error(s)
  STDERR.puts 'Error: #{s}'
  exit 1
end

# Default request and response.  These can be changed to provide defaults.
# Headers present in WAS will *overwrite* headers defined here.  At present,
# there is no support for multiple header with the same name.

DEFAULT_REQUEST = {
  method:   'GET',
  uri:      '/was_to_pb_default',
  protocol: 'HTTP/1.1',
  headers:  {},
  body:     ''
}

DEFAULT_RESPONSE = {
  protocol: 'HTTP/1.1',
  status:   '200',
  message:  'OK',
  headers:  {},
  body:     ''
}

def handle_base64(element)
  if element.attributes['base64'] == 'true'
    Base64.decode64(element.text)
  else
    element.text
  end
end

was = REXML::Document.new STDIN
n = 0
was.elements.each('/WAS_WEBAPP_REPORT/RESULTS/WEB_APPLICATION') do |app|
  id = app.elements['ID'].text
  app.elements.each('VULNERABILITY_LIST/VULNERABILITY/PAYLOADS/PAYLOAD') do |payload|
    num = payload.elements['NUM'].text

    request = payload.elements['REQUEST']
    out_request = DEFAULT_REQUEST.dup

    out_request[:method] = request.elements['METHOD'].text if request.elements['METHOD']
    out_request[:uri] = request.elements['URL'].text if request.elements['URL']
    request.elements.each('HEADERS/HEADER') do |header|
      out_request[:headers][header.attributes['KEY']] = header.text
    end
    out_request[:body] = handle_base64(request.elements['CONTENTS']) if request.elements['CONTENTS']

    response = payload.elements['RESPONSE']
    out_response = DEFAULT_RESPONSE.dup

    response.elements.each('HEADERS/HEADER') do |header|
      out_response[:headers][header.attributes['KEY']] = header.text
    end
    out_response[:body] = handle_base64(response.elements['CONTENTS']) if response.elements['CONTENTS']

    clipp = ClippScript::transaction(id: "WasToPB:#{id}:#{num}") do |t|
      t.request(out_request)
      t.response(out_response)
    end
    print IronBee::CLIPP::HashToPB.hash_to_pb(clipp)

    n += 1
  end
end

STDERR.puts "Generated #{n} connections."