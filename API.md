API
===

The core idea of lumail2, and the reason for writing it, is that everything is an object.

We have three groups of objects:

* The maildir
* The message
* The utilities


Maildir
-------

Constructor:

    maildir = Maildir.new( "./Maildir" )

The Maildir object has the following methods:

* `path()`
    * Returns the path to the Maildir - what it was constructed with.
* `messages()`
	* Returns an array of Message-objects, one for each message in the maildir.
* `total_messages()`
	* Returns the count of messages in the maildir.
* `unread_messages()`
	* Returns the count of unread/new messages in the maildir.
* `exists`
	* Returns `true` if the Maildir exists.


Message
-------

Constructor:

     message = Message.new( "path/to/message" )

Header access can be achived via `header` to read a single header, or `headers` to return a table of all present headers, and their values.

Flags can be accessed via the `flags()` method, which also allows them to be updated.

The `parts()` method allow a message to be examined, for MIME-parts.

    parts = message:parts()

This returns an array of `MessagePart` objects.  The `MessagePart` object
contains methods:

* type()
    * Returns the content-type of the MIME-part
* content()
	* Returns the content of the part.
* is_attachment()
	* Returns `true` if the part represents an attachment, false otherwise
* filename()
	* Returns the name of the attachment, if `is_attachment` returned true.

See [show_message.lua](show_message.lua) for an example use-case of this method.


Utilities
---------