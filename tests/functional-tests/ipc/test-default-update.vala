using Tracker.Sparql;

private static int res;
private static MainLoop loop;

private void test_async () {
	Connection c = null;

	// Test async
	print ("Getting connection async\n");
	loop = new MainLoop (null, false);
	Connection.get_async.begin (null, (o, res) => {
		try {
			c = Connection.get_async.end (res);
			loop.quit ();
		} catch (GLib.Error e) {
			warning ("Couldn't perform test: %s", e.message);
		}
	});
	loop.run ();

	print ("Got it %p\n", c);

	print ("Creating app with connection\n");
	TestApp app = new TestApp (c);

	print ("Running app\n");
	res = app.run();

	print ("\n");
}

private void test_sync () {
	try {
		Connection c;

		// Test async
		print ("Getting connection\n");
		c = Connection.get ();

		print ("Got it %p\n", c);

		print ("Creating app with connection\n");
		TestApp app = new TestApp (c);

		print ("Running app\n");
		res = app.run();
	} catch (GLib.Error e) {
		warning ("Couldn't perform test: %s", e.message);
	}

	print ("\n");
}

int
main( string[] args )
{
	print ("Starting...\n");

	test_sync ();

	if (res < 0) {
		return res;
	}

	// Do async second
	test_async ();

	return res;
}
