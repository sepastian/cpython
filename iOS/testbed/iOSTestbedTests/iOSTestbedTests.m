#import <XCTest/XCTest.h>
#import <Python/Python.h>

@interface iOSTestbedTests : XCTestCase

@end

@implementation iOSTestbedTests


- (void)testPython {
    const char **argv;
    int exit_code;
    int failed;
    PyStatus status;
    PyPreConfig preconfig;
    PyConfig config;
    PyObject *sys_module;
    PyObject *sys_path_attr;
    NSArray *test_args;
    NSString *python_home;
    NSString *path;
    wchar_t *wtmp_str;

    NSString *resourcePath = [[NSBundle mainBundle] resourcePath];

    // Disable all color, as the Xcode log can't display color
    setenv("NO_COLOR", "1", true);

    // Arguments to pass into the test suite runner.
    // argv[0] must identify the process; any subsequent arg
    // will be handled as if it were an argument to `python -m test`
    test_args = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"TestArgs"];
    if (test_args == NULL) {
        NSLog(@"Unable to identify test arguments.");
    }
    argv = malloc(sizeof(char *) * ([test_args count] + 1));
    argv[0] = "iOSTestbed";
    for (int i = 1; i < [test_args count]; i++) {
        argv[i] = [[test_args objectAtIndex:i] UTF8String];
    }
    NSLog(@"Test command: %@", test_args);

    // Generate an isolated Python configuration.
    NSLog(@"Configuring isolated Python...");
    PyPreConfig_InitIsolatedConfig(&preconfig);
    PyConfig_InitIsolatedConfig(&config);

    // Configure the Python interpreter:
    // Enforce UTF-8 encoding for stderr, stdout, file-system encoding and locale.
    // See https://docs.python.org/3/library/os.html#python-utf-8-mode.
    preconfig.utf8_mode = 1;
    // Use the system logger for stdout/err
    config.use_system_logger = 1;
    // Don't buffer stdio. We want output to appears in the log immediately
    config.buffered_stdio = 0;
    // Don't write bytecode; we can't modify the app bundle
    // after it has been signed.
    config.write_bytecode = 0;
    // Ensure that signal handlers are installed
    config.install_signal_handlers = 1;
    // Run the test module.
    config.run_module = Py_DecodeLocale([[test_args objectAtIndex:0] UTF8String], NULL);
    // For debugging - enable verbose mode.
    // config.verbose = 1;

    NSLog(@"Pre-initializing Python runtime...");
    status = Py_PreInitialize(&preconfig);
    if (PyStatus_Exception(status)) {
        XCTFail(@"Unable to pre-initialize Python interpreter: %s", status.err_msg);
        PyConfig_Clear(&config);
        return;
    }

    // Set the home for the Python interpreter
    python_home = [NSString stringWithFormat:@"%@/python", resourcePath, nil];
    NSLog(@"PythonHome: %@", python_home);
    wtmp_str = Py_DecodeLocale([python_home UTF8String], NULL);
    status = PyConfig_SetString(&config, &config.home, wtmp_str);
    if (PyStatus_Exception(status)) {
        XCTFail(@"Unable to set PYTHONHOME: %s", status.err_msg);
        PyConfig_Clear(&config);
        return;
    }
    PyMem_RawFree(wtmp_str);

    // Read the site config
    status = PyConfig_Read(&config);
    if (PyStatus_Exception(status)) {
        XCTFail(@"Unable to read site config: %s", status.err_msg);
        PyConfig_Clear(&config);
        return;
    }

    NSLog(@"Configure argc/argv...");
    status = PyConfig_SetBytesArgv(&config, [test_args count], (char**) argv);
    if (PyStatus_Exception(status)) {
        XCTFail(@"Unable to configure argc/argv: %s", status.err_msg);
        PyConfig_Clear(&config);
        return;
    }

    NSLog(@"Initializing Python runtime...");
    status = Py_InitializeFromConfig(&config);
    if (PyStatus_Exception(status)) {
        XCTFail(@"Unable to initialize Python interpreter: %s", status.err_msg);
        PyConfig_Clear(&config);
        return;
    }

    sys_module = PyImport_ImportModule("sys");
    if (sys_module == NULL) {
        XCTFail(@"Could not import sys module");
        return;
    }

    sys_path_attr = PyObject_GetAttrString(sys_module, "path");
    if (sys_path_attr == NULL) {
        XCTFail(@"Could not access sys.path");
        return;
    }

    // Add the app packages path
    path = [NSString stringWithFormat:@"%@/app_packages", resourcePath, nil];
    NSLog(@"App packages path: %@", path);
    wtmp_str = Py_DecodeLocale([path UTF8String], NULL);
    failed = PyList_Insert(sys_path_attr, 0, PyUnicode_FromString([path UTF8String]));
    if (failed) {
        XCTFail(@"Unable to add app packages to sys.path");
        return;
    }
    PyMem_RawFree(wtmp_str);

    path = [NSString stringWithFormat:@"%@/app", resourcePath, nil];
    NSLog(@"App path: %@", path);
    wtmp_str = Py_DecodeLocale([path UTF8String], NULL);
    failed = PyList_Insert(sys_path_attr, 0, PyUnicode_FromString([path UTF8String]));
    if (failed) {
        XCTFail(@"Unable to add app to sys.path");
        return;
    }
    PyMem_RawFree(wtmp_str);

    // Ensure the working directory is the app folder.
    chdir([path UTF8String]);

    // Start the test suite. Print a separator to differentiate Python startup logs from app logs
    NSLog(@"---------------------------------------------------------------------------");

    exit_code = Py_RunMain();
    XCTAssertEqual(exit_code, 0, @"Test suite did not pass");

    NSLog(@"---------------------------------------------------------------------------");

    Py_Finalize();
}


@end
